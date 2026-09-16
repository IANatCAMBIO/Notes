# GTK4 spike

Phase 0 of `GTK4_MIGRATION.md`: a throwaway window that measures the three
things the port estimate depends on and prints the results to stdout.

- **Q1** — deprecated `GtkTreeView` + `GtkDropTarget`: drag a folder row
  onto / before / after another.  Expect `drop #1 ... after N motion calls`
  exactly once per drag, with the release coordinates.
- **Q2** — `gtk_text_view_add_overlay`: scroll the right pane.  The
  `delta` column must stay constant across all scroll positions.  Its value
  tells you what the view does with its top margin.
- **Q3** — Cmd/Ctrl-click two rows, then press-and-drag one of them.
  Expect `2 row(s) selected at drag start`.

`make run`.  Nothing to install.  See `BUILD.md`.
