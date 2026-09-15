# Building the spike

One C file, one dependency.

- **Toolchain**: any C11 compiler (clang from Xcode CLT on macOS, gcc on
  Linux).
- **GTK 4** ≥ 4.10 (uses `gtk_css_provider_load_from_string`):
  - macOS: `sudo port install gtk4 +quartz`, then
    `export PATH=/opt/local/bin:$PATH` so `pkg-config` finds `gtk4.pc`
  - Debian/Ubuntu/XFCE: `apt install libgtk-4-dev`

Targets: `make` (build), `make run`, `make clean`.
