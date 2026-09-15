# Building Notes

One binary, `./notes`, from `src/*.c`; no generated code, no build system
beyond GNU make.  `README.md` has the one-paragraph version; this is the
rest.

## Toolchain

- A C11 compiler: clang (Xcode command-line tools) on macOS, gcc or clang
  on Linux.  The Makefile uses `cc`.
- GNU make.  macOS ships 3.81, which the Makefile is written for (no
  `$(file …)`, no `!=`).
- `pkg-config` (`pkgconf` on MacPorts) — it is how the compiler and
  linker flags for GTK and SQLite are found.  MacPorts installs it in
  `/opt/local/bin`, which may not be on `PATH`: `export
  PATH=/opt/local/bin:$PATH` first.

## Libraries

| Library | Version | Purpose | MacPorts | Debian/Ubuntu |
|---|---|---|---|---|
| GTK | 4.10+ (4.22 developed against) | the toolkit | `gtk4 +quartz` | `libgtk-4-dev` |
| SQLite | 3 | the database | `sqlite3` | `libsqlite3-dev` |
| librsvg (optional) | any | renders the bundled symbolic SVG tree arrows; without it they fall back to GTK's own | `librsvg` | `librsvg2-common` |

The `+quartz` variant is the native macOS backend.  The native macOS
menubar comes from GTK itself; no other library is involved.  GTK 4
renders through OpenGL or Vulkan; where only software GL exists (some
VMs), run with `GSK_RENDERER=cairo`.

GTK's DEPRECATED-but-present API (the GtkTreeView family, GtkIconView,
GtkDialog, …) is used on purpose and each site is marked; the build is
`-Wall -Wextra` clean and no global deprecation flag is set.  See
`CLAUDE.md` § Build & run.

## Targets

| Target | Does |
|---|---|
| `make` | builds `./notes` (objects in `build/`) |
| `make run` | builds and runs it against the database `notes.ini` names — the real one |
| `make run-dev` | builds and runs it in `dev/`: its own ini (seeded from `notes.ini.defaults`), a throwaway database with a few notes, and a guard that refuses to start unless that ini points at `dev/db` |
| `make clean-dev` | discards the sandbox |
| `make test` | builds and runs `tests/test_document.c` against GLib alone (no GTK on the line): the block model's round trips, operations, undo and a fuzz |
| `make bnbf-scan` | builds `build/bnbf-scan`, which round-trips every note blob in a database COPY through the block model and reports what changed (BLOCK_MODEL.md) |
| `make ui-test` | builds `build/ui-probe` (tests/ui_probe.c) and runs every script in `tests/ui/` through a real note view, checking blocks and caret and rendering each to `build/ui-*.png`; needs a display |
| `make clean` | removes `build/` and the binary |
| `make app` | macOS: `dist/Notes.app` (icon via `sips`/`iconutil`; NOT self-contained — it links MacPorts' GTK dynamically) |
| `make deb` / `make rpm` | Linux packages; build ON the target distribution (`dpkg-deb` / `rpmbuild`); they install to `/opt/notes` plus a `/usr/bin/notes` wrapper |

The version is the one line in `VERSION`; it is baked into the binary,
the package filenames and the `.app`'s `Info.plist`.

## Platform notes

- **macOS**: the app menu (About / Preferences / Quit) and the menubar are
  native; Settings can move the menubar back into the window.  Two benign
  GTK criticals are filtered in `main.c` (`quartz_log_filter`).
- **Linux/XFCE**: the menubar is drawn at the top of the library window
  (GtkApplicationWindow); the `.deb` depends on `libgtk-4-1` and
  `libsqlite3-0`.
