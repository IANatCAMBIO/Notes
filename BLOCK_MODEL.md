# Block document model — design for the note engine's replacement

Written 2026-09-15 on the `gtk4` branch, after the day that produced
D14/D17/D23/D27/D29.  This is a SPEC, not a log: it says what `OnDocument`
and the drawn view are, so that step 1 below can start from an API rather
than from a conversation.  Running state, once work starts, goes in
GTK4_MIGRATION.md's session log like everything else on this branch.

## The problem, stated once

The note's runtime representation is a `GtkTextBuffer`, and the format
(BNBF, `serialize.h`) knows things the buffer cannot express.  So the
engine (`src/note_view.c`, 4.8k lines) re-derives them, and hosts widgets
inside the view for the rest:

| The format says               | The buffer holds                          | Consequence in `note_view.c`                          |
|-------------------------------|-------------------------------------------|-------------------------------------------------------|
| paragraph style H1/H2/code/list | a tag stretched over the line INCLUDING its newline | `line_span`, D20 (join keeps the first style), D30 (last line needs a newline), quirk #19/#20 |
| list item                     | literal "• " / "12. " characters + a tag  | prefix parser shared with export/CLI, renumbering pass, Enter continuation |
| task checkbox (CHECK record)  | an anchor + a space + a tag               | native GtkCheckButton per line, quirk #17 (rise), state as anchor object data |
| image (IMAGE record)          | an anchor carrying a pixbuf as object data | a widget per image, D27 (height race), D29 (unclickable under an overlay) |
| table (TABLE record)          | an anchor carrying an `OnTable` + a GtkGrid of N×M `GtkTextView`s | D23 (focus fight), D25 (no width request), D27, D29, `editor_gate_widget` per cell |
| action item                   | a line whose first char is '!'            | derived `on-action` tag re-applied on every insert/delete/paste/undo; identity via a `GtkTextMark` per line, pruned in delete-range BEFORE (see CLAUDE.md "STABLE identity") |
| title                         | line 0                                    | two derived tags + a CSS class for the empty case (quirk #19) |
| code block "copy"             | nothing                                   | an OVERLAY widget per block, pooled because D17 |
| undo                          | GtkTextBuffer's undo is text-only         | whole-buffer snapshots (`UndoSnap`), grouped by typing pause |

Every row is the same gap.  The four D-numbers that cost most of
2026-09-15 are all "a live widget inside a GtkTextView", the least
exercised corner of the toolkit, and fixing them is patching GTK from
outside.  The rest of the engine (inline styles, #tags, find) has been
stable since GTK3 — those bugs were ours and were fixed once.

## The design, in two sentences

The note is an **`OnDocument`**: an array of blocks (paragraph, heading,
list item, code line, image, table), each owning its text and its runs of
inline flags — GLib only, no GTK, headlessly testable, and a near-1:1
image of the BNBF record stream.  It is shown by **one custom
`GtkWidget`** that lays each text block out with Pango, draws text,
images, tables and code blocks itself from that layout, and owns the
caret, the selection, input methods and the clipboard — no widget is ever
placed inside another widget.

### Why Pango, and why this is not slower than GtkTextView

GTK 4.22 requires `pango >= 1.56` and `pangocairo` (`pkg-config
--print-requires gtk4`).  Every GtkLabel, GtkText and GtkTextView builds a
`PangoLayout` and hands it to GSK with `gtk_snapshot_append_layout()`;
what GTK4 changed against GTK3 is the back half — the GL/Vulkan renderers
rasterise glyphs into a texture atlas once and composite on the GPU
instead of cairo painting text per frame.  Layout, shaping (HarfBuzz
inside Pango), line breaking, bidi and cursor geometry are Pango in both.
A custom widget calling `gtk_snapshot_append_layout` is therefore on the
identical path as GtkTextView, under every renderer (D26's cairo renderer
included), and gets HarfBuzz shaping, grapheme-correct cursor motion
(`pango_layout_move_cursor_visually`), word boundaries
(`pango_layout_get_log_attrs`) and inline object slots (`PangoAttrShape`)
for free.  The desktop's own toolkit (XFCE = GTK3) is irrelevant: the app
links its own GTK4 and Pango.

### The three renderings considered

| | A. widget per block (Notion) | B. GtkTextView per text run, widgets between | **C. one drawn widget** |
|---|---|---|---|
| text editing            | GTK's, per block | GTK's | ours, on Pango |
| tables / images / code  | peer widgets     | peer widgets | drawn, hit-tested by us |
| cross-block selection   | impossible (Notion's "select blocks" mode is the admission) | broken at every non-text block | trivial: two positions in one model |
| ⌘A, arrows across blocks, find, copy across blocks | hand-rolled, awkward | hand-rolled | natural |
| widgets inside widgets  | none | text views remain | none |
| GTK-internal exposure   | GtkListView recycling + N text views | GtkTextView | none — Pango, GdkTexture, controllers, GtkIMContext |
| keystroke cost          | one block's layout | one view's revalidate | one block's `PangoLayout` |
| what we must build      | selection model | most of C anyway | caret, selection, IM, clipboard, a11y |

C is the design.  A is what "block model" usually means and is the one to
reject even with unlimited time: it trades the anchored-widget problem for
a selection problem nobody has solved.  B keeps the thing being removed.

## OnDocument — `src/document.[ch]`

GLib only.  No GTK type appears in the header; the image block holds PNG
bytes and an opaque decoded-pixels slot the view fills.

### Blocks

```c
typedef enum {
    ON_BLOCK_PARA,                   /* body text                            */
    ON_BLOCK_H1, ON_BLOCK_H2,        /* headings                             */
    ON_BLOCK_BULLET, ON_BLOCK_NUMBER,/* list items                           */
    ON_BLOCK_CHECK,                  /* task item: `checked` is the state    */
    ON_BLOCK_CODE,                   /* ONE code LINE (see "code blocks")    */
    ON_BLOCK_IMAGE,                  /* an image on a line of its own        */
    ON_BLOCK_TABLE,                  /* rows × cols of text cells            */
} OnBlockKind;

typedef struct {                     /* one run of identically-styled text  */
    gsize   len;                     /* bytes; runs tile the text exactly    */
    guint32 flags;                   /* ON_FMT_INLINE_MASK bits only         */
} OnRun;

typedef struct {                     /* text of a PARA/H*/list/CODE block,
                                        or of one table cell                 */
    GString *text;                   /* UTF-8, NO newline, NO list prefix,
                                        NO checkbox character                 */
    GArray  *runs;                   /* OnRun, adjacent equal flags merged   */
    GArray  *objects;                /* OnInlineObject, by byte offset:
                                        inline images (below)                */
} OnText;

typedef struct OnBlock {
    OnBlockKind kind;
    OnText     *text;                /* every kind but IMAGE and TABLE       */
    gboolean    checked;             /* CHECK                                */
    /* IMAGE */
    GBytes     *png;                 /* the stored bytes, VERBATIM (never
                                        re-encoded — CLAUDE.md "Images are
                                        never re-encoded")                   */
    guint32     display_width;       /* 0 = default thumbnail sizing         */
    gpointer    pixels;              /* view-owned decode (a GdkTexture);
                                        NULL until the view needs it          */
    /* TABLE */
    gint        rows, cols;
    gboolean    header;
    OnText    **cells;               /* rows*cols, row-major                 */
    /* identity — NOT serialized, survives every edit of this block         */
    gint64      action_uid;          /* 0 = none yet; see "action items"     */
} OnBlock;

struct OnDocument {
    GPtrArray *blocks;               /* OnBlock*, at least one               */
    OnUndoLog *undo;                 /* see "operations and undo"            */
    /* observers, see below */
};
```

**Text blocks have no newline** — the block boundary IS the newline.  This
single fact retires `line_span`, D20, D30, quirk #19 and quirk #20: a
style is a property of the block, an empty block has a kind like any
other, and joining two blocks is `text_append` + `block_remove`, which
keeps the first block's kind because nothing else is possible.

**List prefixes and checkbox characters are not in the text.**  The
serializer re-emits them (below) so every other reader of BNBF — export,
`body_text`, `note cat`, the GTK3 build on `main` — sees the same bytes it
always did.  A numbered item's number is its position in the run of
consecutive `ON_BLOCK_NUMBER` blocks: renumbering is what the layout
prints, not a pass over the text.

**Inline images.**  Today an image is a CHARACTER in a line — a user pastes
three screenshots in a row without pressing Enter, and BNBF allows an
IMAGE record mid-run.  Two kinds of image therefore exist:

- `ON_BLOCK_IMAGE`: an IMAGE record with nothing but newlines around it —
  the common case (paste, Enter, paste).  A block, laid out by us.
- an inline object in `OnText.objects` (`OnInlineObject { gsize offset;
  OnBlock *image; }`): an IMAGE record with text on the same line.  The
  text carries U+FFFC at `offset`; the layout gives that character a
  `PangoAttrShape` of the display size, so Pango wraps around it and the
  caret treats it as one character; the view draws the texture at
  `pango_layout_index_to_pos`.  This is exactly how GtkTextView implements
  anchors internally — the mechanism is public Pango.

The inline form exists for FIDELITY (a note that has it round-trips
byte-identical); the editor's own insert always makes the block form
(splitting the paragraph at the caret), and the image context menu has no
"make inline".  If the dev/live database scan in step 1 finds zero
mid-line images, drop the inline form before writing the view.

**Code blocks are runs of `ON_BLOCK_CODE` blocks**, one per line, exactly as
BNBF stores them (one TEXT run per line under ON_FMT_CODEBLOCK).  The
layout treats consecutive CODE blocks as one visual block: one shaded
rectangle, one gutter numbering from 1, one "copy" word at the top right.
No block ever contains other blocks except TABLE → cells, and a cell is a
plain `OnText`, never a block — the nesting stops at depth one, which is
what makes positions simple.

### Positions

```c
typedef struct {
    gint  block;                     /* index into blocks                    */
    gint  cell;                      /* row*cols+col inside a TABLE, else -1 */
    gsize offset;                    /* BYTE offset into that OnText; for an
                                        IMAGE block 0 = before, 1 = after    */
} OnPos;
```

The caret is one `OnPos`; the selection is two (anchor + caret), ordered
by `on_pos_cmp`.  Selecting from a paragraph across a table into another
paragraph selects the WHOLE table (a table block is one unit from
outside); a selection whose two ends are inside the same cell is a text
selection in that cell; two ends in different cells of one table is a
CELL-RANGE selection (spreadsheet style — v1 supports it for delete and
copy-as-text only).

### Derived properties — computed, never stored

- **Title**: block 0, whatever it holds (`first_line_title`).
- **Action item**: a text block of kind PARA/H*/list whose text starts
  with '!' — the format contract `on_note_extract_actions` defines,
  unchanged, because the CLI and the `action_items` mirror depend on it.
  `on_document_actions()` returns the ordered list in O(blocks) with no
  allocation beyond the list.
- **#tags**: `ON_FMT_TAG` runs, as today.
- **Empty note**: one PARA block with empty text.  The document is never
  zero blocks.

Nothing about these is re-derived on edit; the layout reads them when it
lays the block out, and the block is the only thing invalidated.

### Action-item identity

`OnBlock.action_uid` replaces the per-line `GtkTextMark` + prune-in-
delete-range machinery.  A block struct survives every edit to its text,
so a rewording keeps its uid with no mark to protect; the four-pass match
in `on_db_note_set_actions` (text → hint → ord → fresh) is unchanged, it
just gets its hint from the block.  Cut-and-paste of a line still makes a
new block (uid 0) and is still caught by the text pass.  Undo restores
the block WITH its uid (the inverse of `block_remove` re-inserts the same
struct).

### Operations and undo

Every mutation is one of a closed set of operations, each with an
inverse, applied through `on_document_apply()`:

```
insert_text   (pos, text, flags)   ↔ delete_text (pos, len)
delete_text   (pos, len)           ↔ insert_text (pos, saved text+runs, saved objects)
set_flags     (pos, len, mask, on) ↔ set_flags with the saved runs
split_block   (pos)                ↔ join_blocks (block)
join_blocks   (block)              ↔ split_block (saved pos)
set_kind      (block, kind)        ↔ set_kind (block, old kind)
set_checked   (block, bool)        ↔ set_checked (block, old)
insert_block  (index, block)       ↔ remove_block (index)
remove_block  (index)              ↔ insert_block (index, saved block)
table_set_cell/insert_row/remove_row/insert_col/remove_col/set_header
                                   ↔ their obvious inverses
```

Compound edits (typing over a selection = delete + insert; Enter in a
list = split + set_kind; Backspace at a block start = join) are one
undo GROUP.  Groups also coalesce consecutive typing under the SAME rule
`note_view.c` uses today (pause timer, `UNDO_MAX_SENTENCES`), so the
user-visible granularity does not change.  An undo group is a list of
ops; undo applies the inverses in reverse; redo re-applies.  No snapshot
is ever taken — an undo step on a note with twenty screenshots costs the
text it touched, not the note.

`on_document_apply` is also where the observer fires: `block_changed(i)`,
`blocks_inserted(i, n)`, `blocks_removed(i, n)`, `structure_changed()`
(table shape).  The layout subscribes; nothing else needs to, since the
host learns "edited" from the view.

### Invariants (asserted in debug builds, tested headlessly)

- `blocks->len >= 1`.
- Every `OnText`'s runs sum to `text->len`; no run has `len == 0`; no two
  adjacent runs have equal flags; run boundaries fall on UTF-8 character
  boundaries.
- Every inline object's `offset` addresses a U+FFFC in the text, and every
  U+FFFC has an object.
- Cells of a TABLE are never NULL; `rows >= 1 && cols >= 1`.
- Text contains no '\n'.
- `action_uid` is 0 or unique across the document.

## BNBF ⇄ document — `src/serialize.[ch]`

The public contract of the FORMAT does not change.  Version stays 5;
readers on `main` keep reading what this branch writes.  What changes is
that `on_note_serialize` / `on_note_deserialize` take an `OnDocument`, and
the buffer-walking half of serialize.c (`on_buffer_walk`, `on_flags_at_iter`,
`OnFlagRun`, the anchor object-data accessors, `on_buffer_ensure_tags`) is
deleted.  Every record walk that exists today BECAUSE deserializing was
expensive — `on_note_extract_text`, `on_note_extract_actions`,
`on_note_count_images`, `on_note_image_nth`, `on_png_probe_size` — is
kept as is: they read the blob, and the blob is unchanged.  (A document
load decodes no PNG either — `pixels` is filled lazily by the view — so
a document IS now as cheap as a record walk; the walks stay because the
CLI and media browser already have their contract and there is nothing
to gain by touching them.)

Mapping, load direction:

| BNBF                                            | document                                                    |
|-------------------------------------------------|-------------------------------------------------------------|
| TEXT runs up to and including a '\n'            | one text block; kind from the ON_FMT_PARA_MASK bits of the runs (the LINE's style is the style of its newline run, as today); inline flags → runs; the literal list prefix / the leading " " after a CHECK is consumed, not stored |
| CHECK at line start                             | the block's kind is CHECK, `checked` from the record        |
| IMAGE with only '\n' (or blob ends) either side | `ON_BLOCK_IMAGE`                                            |
| IMAGE with text on its line                     | inline object; U+FFFC inserted at that offset               |
| TABLE                                           | `ON_BLOCK_TABLE`; a TABLE mid-line SPLITS the line into two text blocks around it (the one lossy-on-load case; the scan in step 1 counts how many notes hit it — expected zero, the editor never made one on purpose) |
| CHECK not at a line start                       | normalized to the start of its line (same note as TABLE)    |
| a trailing run with no '\n'                     | the last block (a document always ends with a block, never with a newline block — `on_note_serialize` writes no trailing '\n' when the last block is text, matching today's bytes) |

Save direction is the inverse, emitting the list prefix / CHECK + " " /
paragraph flags on every run of the line exactly as `on_buffer_walk` did.
**Acceptance test for step 1**: for every blob in the dev database and in
a copy of the live one, `serialize(deserialize(blob)) == blob` byte for
byte, except for notes the scan reports as normalized (table mid-line,
check mid-line, a stale numbered prefix), each of which is listed with
its id.  This test is `make test`; it runs headless.

## Layout — `src/doc_layout.[ch]`

One `OnDocLayout` per view, subscribed to the document.  Per block:

```c
typedef struct {
    PangoLayout *layout;             /* text blocks and cells; NULL = stale  */
    gint         height;             /* logical px incl. block spacing       */
    gint         y;                  /* top, from the document's top         */
    /* IMAGE: fitted w/h; TABLE: column widths + row heights + cell layouts */
} OnBlockLayout;
```

- **Attributes come from the block**: weight/style/underline/strike from
  runs; `ON_FMT_TAG` → the tag colour; heading scale on H1/H2; monospace
  on CODE; centred + scaled on block 0 when `first_line_title`; the
  action tint on '!' blocks; `PangoAttrShape` per inline object; search
  hits as a background attr; the macOS emoji letter-spacing (D24 recipe,
  emoji only, `#ifdef __APPLE__`); the IM preedit's attrs on the caret
  block.  All of quirk #19/#20 vanish: an empty block gets the same attrs
  as a full one because attrs are built from the BLOCK, not from a tag
  over characters, and there is no "derived tag over a real tag" — scale
  is set once per block.
- **Width**: view width less margins less the block's indent (list level,
  code gutter).  A width change invalidates everything; nothing else
  does.
- **Heights and offsets**: `y` is a prefix sum recomputed from the first
  changed block to the end — integer adds over a few thousand blocks,
  nothing to optimise.  Visible range = binary search on `y`.
- **Laziness**: lay out eagerly on load and on width change (a
  2,000-block note is a few tens of ms of Pango — MEASURE this in step 3
  on the largest note in the live database; go lazy with estimated
  heights, GtkTextView's own trick, only if that number is bad).  Per
  keystroke, exactly one block re-lays out.
- **Tables**: column width = widest line in the column measured with a
  probe layout, capped at `TABLE_COL_MAX` (320 px, D25's rule), cells
  wrap past the cap; row height = tallest cell.  This is `table_fit_columns`
  today, minus the part that fights GtkTextView's measure (D25) and the
  validation race (D27) — the layout IS the measurement.
- **Code blocks**: consecutive CODE blocks share one gutter width (digits
  of the run's line count) and one shaded rectangle.
- **Hit testing**: `on_doc_layout_pos_at(x, y) → OnPos` (block by `y`,
  then `pango_layout_xy_to_index`, cell first for tables); and
  `on_doc_layout_rect_of(pos)` for the caret and for scroll-to-caret.
  Also the non-text targets: `copy word of code run N`, `image block`,
  `inline image`, `checkbox of block N` — an enum + index, so the view's
  click handler is one switch.
- **Two painters, one layout**: `on_doc_layout_snapshot()` for the view
  (`gtk_snapshot_append_layout/texture/color`) and
  `on_doc_layout_cairo()` for offscreen use.  The grid thumbnail
  (`render_note_thumb`) needs neither — first image + body text come
  straight off the document — but export-to-image or printing would.

## View — `src/doc_view.[ch]`

`OnDocView`, a `GtkWidget` subclass implementing `GtkScrollable` (vertical
adjustment; horizontal fixed at the viewport width) and `GtkAccessibleText`.
It owns exactly the state GtkTextView owned for us and nothing it did not:

- **Drawing** (`snapshot`): background; for each visible block: code
  shading / table borders / selection rectangles (`pango_layout_line_get_x_ranges`
  across the selected span) BELOW; the layout / the texture ABOVE; gutter
  numbers and the "copy" word; the caret (a 1-px rect at
  `pango_layout_get_cursor_pos`, blinking on GTK's `gtk-cursor-blink*`
  settings, shown only while focused).  Colours come from the widget's
  own style context, so quirk #23 (grey text in backdrop) cannot happen —
  we choose what backdrop looks like.
- **Pointer**: `GtkGestureClick` (1/2/3 presses = caret / word / block,
  words from `pango_layout_get_log_attrs`), `GtkGestureDrag` for selection
  with auto-scroll at the edges, `GtkEventControllerMotion` for the cursor
  (text, hand over a copy word / image / checkbox, pointer over a table
  border).  A press on a checkbox toggles `set_checked` — a drawn box, no
  GtkCheckButton, no quirk #17.  A press on a copy word copies the run.  A
  click on an image emits "image-activated" with its ordinal (the host's
  modal viewer, unchanged).  Right-click emits the context menu the host
  already builds through `on_app_menu_popup` (D14 keeps the popover out of
  the view — it was never the view's).
- **Keyboard**: a `GtkShortcutController` bound to a `move-cursor`
  action signal (`GtkMovementStep` + count + extend), the same vocabulary
  GtkTextView uses so the binding table is a transcription: arrows,
  Home/End, Page, word steps (Ctrl on Linux, ⌥ on macOS), line steps (⌘←/→),
  document steps (⌘↑/↓, Ctrl+Home/End), Shift extends everything.
  Backspace/Delete at a block edge join; Enter splits (continuing a list,
  ending it on an empty item, staying in a code run); Tab in a table moves
  cell, elsewhere inserts a tab; Escape does nothing (the host's find box
  and viewer own it).  All editing actions stay on the host's `win.`
  group exactly as now — the view exposes methods, the window binds keys.
- **Input methods**: `gtk_im_multicontext_new()` attached through
  `gtk_event_controller_key_set_im_context`; `commit` → insert_text with
  the current inline flags; `preedit-changed` → the caret block is laid
  out with the preedit string spliced in and its attrs applied (display
  only, not an op); `retrieve-surrounding` / `delete-surrounding` served
  from the caret block.  `gtk_im_context_set_cursor_location` on every
  caret move so the candidate window follows.  Emoji: the host's
  `insert_emoji` becomes "pop GTK's emoji chooser, commit the pick as
  text" — the chooser is a plain widget, not a text-view feature.
- **Clipboard**: `GdkContentProvider` union of `text/plain;charset=utf-8`
  (the selection's plain text, list prefixes rendered, one line per
  block, cells tab-separated) and `application/x-notes-bnbf` (the
  selection serialized as a sub-document — images and tables and their
  state INCLUDED, which fixes "copy/paste within a note drops the
  widget/state" for the first time).  Paste prefers bnbf, then an image
  (`GdkTexture` → PNG bytes → IMAGE block, encoded ONCE and kept as the
  block's `png`), then text.  Primary selection on X11 the same way.
- **Scrolling**: `vadjustment.upper` = total height; scroll-to-caret after
  every op that moved it, computed from `rect_of` — no idle, no race with
  a validation the widget does not have (D27's second half).
- **Accessibility**: `GtkAccessibleText` — contents, caret, selection,
  attributes and extents off the model and layout;
  `gtk_accessible_text_update_*` from the observer.  Done in step 3, not
  deferred: it is a few hundred lines against a defined interface and it
  is the one thing GtkTextView gave us that users of screen readers would
  notice missing.
- **Focus**: the view is the ONLY focusable thing in the note area — no
  cell widgets, no find-entry-versus-cell gate (D19 shrinks to "find
  entry focused → editing actions off").

### What `note_view.h` becomes

The host (`editor_window.c`) keeps its shape.  The contract maps almost
one to one; the differences are all deletions:

| `note_view.h` today                          | after                                                     |
|----------------------------------------------|-----------------------------------------------------------|
| `load(blob)` / `serialize()`                 | same names; the view holds an `OnDocument`                |
| `toggle_inline` / `toggle_paragraph`         | same, as `set_flags` / `set_kind` ops                     |
| `insert_image/table/date/emoji`              | same                                                      |
| `undo` / `redo`                              | same; op log instead of snapshots                         |
| `find` / `find_step`                         | same; hits are per-block ranges                           |
| `image_count/nth/reveal`                     | same; ordinal = image blocks + inline objects in document order, which is IMAGE-record order (the media browser's addressing scheme, unchanged) |
| `settings_changed`                           | same; relayout                                            |
| `take_tags_modified` / `take_actions_modified` | same, set by the ops that can change them (a run gaining or losing ON_FMT_TAG; a '!' or a split/join/remove at a block start) |
| `action_marks_hint` / `action_marks_sync`    | `on_document_action_uids_get/set` — read/write `OnBlock.action_uid`; no marks |
| `on_note_buffer_action_strike/due/text` on a bare GtkTextBuffer | `on_document_action_strike/due/text` on a bare document — the CLI's headless path becomes blob → document → op → blob with no GTK, no offscreen buffer, no `on_buffer_ensure_tags` |
| signals `edited`, `inline-flags-changed`, `image-activated` | same                                                      |
| signal `cell-created`                        | gone (nothing to gate)                                    |
| `on_note_view_action_group` (D14 plumbing)   | gone — context menus are the host's, the view only reports where the click was |

## What else changes, and what does not

- `export.c` walks blocks instead of a deserialized buffer: simpler, and
  the images go out through `OnBlock.png` verbatim (already the rule).
- `cli.c`'s eighteen buffer references are the headless action rewrites
  and `note cat --md`; both move to the document, dropping the GTK
  dependency from every headless command that has one.
- `library_window.c`'s eleven are `render_note_thumb` and the AI-summary
  text; both read the document.
- `search_query`, `body_text`, the media browser, `on_note_image_nth`,
  the CLI's image commands: untouched — they read blobs, and blobs do
  not change.
- `db.c`: untouched.
- GTK3 `main`: untouched; it keeps reading and writing the same BNBF.  A
  note edited on either branch opens on the other.
- Deleted: `note_view.c`, the buffer half of `serialize.c`, every
  `GtkTextChildAnchor` accessor, `editor_gate_widget`'s cell half, the
  copy-link pool, quirks #17/#19/#20/#22/#23, D15/D16/D17/D20/D23/D25/D27/
  D29/D30 (they describe a widget we no longer use; they stay in the
  docs as history with a one-line "superseded by BLOCK_MODEL.md").

## Deliberately not done

- **Text drag-and-drop** inside a note.  GtkTextView had it; nobody has
  asked for it; it is the single most bug-prone feature of a text widget.
  Cut/paste covers it.
- **Nested lists / indentation levels**.  The format has none; the model
  could add a `level` later without touching positions.
- **Spell checking, rich-text drop targets from other apps** (beyond
  images and text).  Same as today.
- **Lazy layout** until measured necessary (above).
- **Mixed-style single characters inside an inline object's line** are
  not a thing: U+FFFC carries the flags of its run and they mean nothing.

## Plan

Each step lands on its own, builds, and passes `make test`.

1. **`document.[ch]` + `serialize` on the document + headless tests.**
   Byte-identical round trip on every blob in the dev database, then on a
   COPY of the live one (`notes backup FILE.db`, then a small `tools/`
   program — never the live file, CLAUDE.md "NEVER run ./notes from the
   repo root").  The scan also answers the two open questions below.
   Deliverable: a passing `make test`, and the normalized-note report.
   `GtkTextView` still renders; nothing user-visible moves.
2. **Consumers onto the document**: `export.c`, the headless action
   rewrites in `cli.c`, `render_note_thumb`, the AI-summary text.  The
   engine keeps running on the buffer; the buffer is built FROM the
   document at load and serialized back at save through a thin, temporary
   `document ↔ buffer` bridge (the old serialize code, renamed, ~300
   lines — it dies in step 3).  Deliverable: `note cat --md`, export and
   the grid thumbnails byte-for-byte / pixel-for-pixel as before, checked
   against `main`.
3. **`doc_layout` + `doc_view` behind `note_view.h`; delete
   `note_view.c`.**  Verified by hand in the sandbox against the existing
   GTK4 checklist in GTK4_MIGRATION.md (menus, shortcuts, tables, code
   blocks, emoji, viewer, DnD of notes in the library is unaffected) plus
   the new ones: cross-block selection incl. over a table and an image;
   copy/paste of a table and an image within and between notes; IM input
   (macOS Japanese, Linux ibus — the preedit path); a screen reader
   reading the caret line (VoiceOver / Orca); the largest note in the
   live database opening, scrolling and typing with no visible hitch —
   measured, numbers into GTK4_MIGRATION.md's Decisions.

## Step 1 — done 2026-09-15

`src/bnbf.[ch]` (the format: flags, reader, writer, tables, the two line
parsers — GLib only), `src/document.[ch]` (the model, loader/saver,
operations, undo, observer, change flags — GLib only),
`tests/test_document.c` (25 tests incl. a 400-op fuzz that undoes and
redoes everything byte for byte; `make test`, zero leaks under `leaks`)
and `tools/bnbf-scan.c` (`make bnbf-scan`).  `serialize.c` now drives the
shared reader and writer instead of its own framing.  The app builds and
the sandbox writes blobs the model reads back identically.

**The scan, on a read-only copy of the live database (1354 notes, 709 MB):
1315 byte-identical, 39 normalized, 0 unexplained, 0 invalid, 0 errors.**
Load 36 ms, save 75 ms for the whole database — the loader copies PNG
bytes and decodes nothing.

What the data taught, and how the spec above was adjusted:

- **`OnBlock.eol_flags`** (not in the spec): a line's newline carries
  inline flags in the blob — the editor tags Enter like any typed
  character — and 7 notes have them.  Meaningless on screen, but the
  model keeps the bits so those notes round-trip exactly; a new block's
  newline takes whatever the caller arms (`on_document_split_block`).
- **The FIRST character decides a line's paragraph style**, not the
  newline's run as first written: that is what the editor renders
  (`line_para_flags` tests the line start), and the 3 `mixed_para` notes
  are 2026-08 pre-D30 lines whose newline missed the H2 — under the
  newline rule they would have LOST their heading.
- **`run_split`**: 37 notes hold two consecutive TEXT records with equal
  flags (builds before 2026-08 wrote the title line as its own record).
  The format allows it; the model has no place for a record boundary and
  the saver writes maximal runs.  Harmless, counted.
- **Inline images stay**: 3 notes hold 6 images that share a line with
  text.  The inline form (U+FFFC + `OnInlineImage`) round-trips them
  exactly and is what the fuzz exercises; the view will draw them via
  `PangoAttrShape` as planned.
- **Table cells are `OnText`** with newlines allowed (the old editor
  wrote multi-line cells; 1 note has a table) and no inline images — the
  one place the "no newline in text" invariant is relaxed.
- **A no-op `set_*` logs no undo step** (`on_document_undo_depth` says
  how many there are, which is also how a host can tell "changed since
  save" without comparing bytes).
- `OnTable` (string cells) survives on the GtkTextBuffer side as the
  anchor payload the reader hands out; the document converts.  It goes
  with serialize.c in step 3.
- Normalizations the loader counts and the saver repairs: a table or
  checkbox mid-line (0 in the data), a check line without its space /
  box / tag (0), a bullet without its prefix (0), a stale number (0),
  a styled prefix (0), unknown flag bits (0).  Every category is a test.

Largest note: 615 blocks (note 734); most text: 73 789 bytes (note 1324)
— the layout's eager target in step 3 is small.

## Step 2 — done 2026-09-15

Every offscreen consumer reads the document; `GtkTextBuffer` now serves
the open editor alone.  `on_note_document_load()` (serialize.c) is the
one preamble; `on_note_buffer_load` / `on_note_deserialize_scaled` and the
CLI's GTK initialisation are gone.

- **`export.c`** walks blocks.  Checked against the pre-change binary on
  the live copy: all 1354 notes' HTML, Markdown and `note cat --md`
  outputs byte-identical except **two notes whose task lines the OLD
  exporter got wrong** — it read a line's style off its first character,
  which for a loaded task line is the untagged checkbox anchor, so every
  `- [ ]` came out as a plain paragraph with a leading space (the same
  code is on `main`).  Export of the whole library: 35 s → 2 s, because
  nothing is decoded.
- **`cli.c`**: `note new/append/set/tag/untag/add-image` build or edit an
  `OnDocument` through the operations; `action done/due/text` go through
  `on_document_action_*` (the editor's live-buffer path is unchanged).
  The same 25-command sequence run through the old and the new binary
  leaves byte-identical blobs, titles, body text, tags and action rows.
- **`render_note_thumb`** takes the first image's bytes and the plain
  text off the document; `on_png_decode_capped` is public and still the
  one decoder.
- The model gained `on_document_title` (the buffer's first-line rule,
  including its quirks — a bullet's prefix is part of the title, a
  whitespace-only line yields the fallback), `on_document_from_text`,
  `on_document_collect_tags` and the three action rewrites, each a unit
  test.  The loader now breaks lines at `\r\n`, `\r` and U+2029 as
  GtkTextBuffer and Pango do (`cr_newlines`: 31 lines in the live data,
  from Windows pastes), which is what made the exports match.
- The AI-summary pane needed nothing: it reads `body_text` and uses a
  GtkTextBuffer only as its display widget.

## Step 3 — done 2026-09-15

`src/doc_layout.[ch]` (1.6 k lines) and a rewritten `src/note_view.[ch]`
(2.7 k, was 4.8 k) replace the GtkTextView engine; `serialize.[ch]` keeps
only the blob walks (0.34 k, was 1.3 k).  The file is named `note_view`,
not `doc_view` as planned: the host's contract (`note_view.h`) kept its
shape, so the file kept its name.  Deviations from the spec above, and
what testing found:

- **Layout is eager and fast enough**: the largest note (615 blocks,
  12 k px) lays out in 18 ms — once images stopped being decoded for
  their size.  `gdk_texture_new_from_bytes` on 17 screenshots was 220 ms;
  the PNG header (IHDR) gives the size for free and the texture is
  decoded when first DRAWN, cached on the block (`OnBlock.pixels`).  The
  73 KB-text note is 64 ms, all Pango; one block per keystroke after.
- **The clipboard does what the spec promised**: `application/x-notes-bnbf`
  beside `text/plain`; a table or image round-trips through copy/paste.
- **No lazy-height machinery, no text DnD, no cell-range editing** beyond
  Tab selecting the whole next cell — as planned.
- **Word steps at a block's end land on the next block's start** (GTK's
  went to the next word's end).  Kept: simpler, and consistent with
  character steps.
- **GtkAccessibleText** is implemented over the note's plain text
  (contents, caret, selection); attributes are reported empty.
- **The keyboard and pointer are function calls too**
  (`on_note_view_feed_key/text/click`): GTK4 has no synthetic input, and
  macOS accessibility clicks never reach a GTK4 window, so
  `tests/ui_probe.c` drives the view through the same code the
  controllers run, renders the window to a PNG and checks blocks and
  caret.  `make ui-test` runs `tests/ui/*.txt` (typing, blocks, objects,
  navigation); the scripts double as the behaviour spec.
- **What the probe cannot drive**: the input method's preedit, the
  context-menu popover, scrolling by wheel, the emoji chooser.  Those are
  verified by hand in the sandbox.

Measured in the sandbox: a real 686 MB library's notes open with 0
Gtk-CRITICALs; the grey-backdrop text (quirk #23), the unclickable
tables (D29), the height race (D27) and the copy-link pool (D17) do not
exist to fix.
