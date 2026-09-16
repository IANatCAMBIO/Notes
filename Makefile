# =============================================================================
# Notes — Makefile
#
# Builds the Notes application (a GTK4 + SQLite notes app written in
# plain C).  Requires GTK4 and SQLite3, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk4 +quartz
#
# Targets:
#     make          — build the `notes` binary
#     make clean    — remove build artifacts (including dist/)
#     make test     — headless tests of the block model (GLib only)
#     make bnbf-scan — build/bnbf-scan, the blob round-trip scan
#     make run      — build and launch the app
#     make app      — macOS .app bundle → dist/Notes.app
#                     (needs the macOS sips/iconutil tools; the bundle
#                     still depends on the MacPorts GTK libraries)
#     make deb      — Debian package → dist/notes_<version>_<arch>.deb
#                     (needs dpkg-deb; build ON a Debian/Ubuntu machine —
#                     the packaged binary is whatever `make` produced)
#     make rpm      — RPM package → dist/notes-<version>-1.<arch>.rpm
#                     (needs rpmbuild; same caveat as deb)
# =============================================================================

# Semantic version — read from the VERSION file, which is the single
# source: it is baked into the binary (ON_VERSION, shown in the About
# dialog), into the .deb/.rpm filenames and into the .app bundle's
# Info.plist (the bundle name itself is unversioned).  Bump the version by
# editing that file, nothing here.  Read with `cat` rather than make 4's
# $(file <...) — the system make on macOS is 3.81, which lacks it.
VERSION  := $(strip $(shell cat VERSION 2>/dev/null))
ifeq ($(VERSION),)
$(error the VERSION file is missing or empty)
endif

# The compiler to use.  clang is the system compiler on macOS.
CC       := cc

# pkg-config binary.  MacPorts installs into /opt/local/bin, which may not be
# on PATH in every shell, so fall back to the absolute path if needed.
PKGCONF  := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# Compiler flags:
#   -std=c11        — use the C11 language standard
#   -Wall -Wextra   — enable a broad set of warnings
#   -g              — include debug symbols
#   plus the include paths for GTK4 and SQLite3 from pkg-config.
#   Deprecated-but-present GTK4 API (the GtkTreeView family, GtkIconView,
#   GtkDialog, …) is used on purpose: those call sites are wrapped in
#   G_GNUC_BEGIN/END_IGNORE_DEPRECATIONS, or a file that lives on them
#   (library_window.c) defines G*_DISABLE_DEPRECATION_WARNINGS at its top,
#   so the build stays warning-clean and the sites stay greppable for the
#   GTK5 migration.  No global -Wno-deprecated-declarations.
CFLAGS   := -std=c11 -Wall -Wextra -g \
            -DON_VERSION='"$(VERSION)"' \
            $(shell $(PKGCONF) --cflags gtk4 sqlite3)

# Linker flags: the GTK4 and SQLite3 libraries from pkg-config, plus libm.
LDFLAGS  := $(shell $(PKGCONF) --libs gtk4 sqlite3) -lm

# All C source files that make up the application.
SRCS     := src/main.c \
            src/app.c \
            src/list_rows.c \
            src/cli.c \
            src/db.c \
            src/ipc.c \
            src/bnbf.c \
            src/document.c \
            src/serialize.c \
            src/doc_layout.c \
            src/note_view.c \
            src/editor_window.c \
            src/image_viewer.c \
            src/library_window.c \
            src/media_window.c \
            src/search_query.c \
            src/search_window.c \
            src/settings_window.c \
            src/export.c \
            src/backup.c

# Object files derived from the source list (build/ mirrors src/).
OBJS     := $(SRCS:src/%.c=build/%.o)

# The final executable name.
BIN      := notes

# Default target: build the application binary.
all: $(BIN)

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/, recreating the directory if
# needed.  Every object depends on all headers for simplicity (the project
# is small enough that full rebuilds on header change are cheap), and on
# the Makefile and the VERSION file so a version bump recompiles the
# baked-in ON_VERSION.
build/%.o: src/%.c $(wildcard src/*.h) Makefile VERSION
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# ---------------------------------------------------------------------------
# Development sandbox.  `make run-dev` runs the freshly built binary from
# dev/ — its OWN notes.ini (db_dir = dev/db) and its own throwaway database,
# seeded with a few notes on first use — so a development run can never open
# the real database the binary-adjacent notes.ini points at.  The binary and
# icons are symlinks, so it is always the current build; the ini and db are
# real files that persist between runs (`make clean-dev` discards them).
# The IPC socket is per database, so a dev GUI and a real one may run at the
# same time without either's CLI commands reaching the other.
# ---------------------------------------------------------------------------
DEV_DIR := dev
DEV_DB  := $(DEV_DIR)/db/$(shell grep -o '"[a-z]*\.db"' src/db.h | tr -d '"')

$(DEV_DIR)/notes.ini: notes.ini.defaults | $(DEV_DIR)
	sed 's|^db_dir=.*|db_dir=$(CURDIR)/$(DEV_DIR)/db|' notes.ini.defaults > $@

$(DEV_DIR):
	mkdir -p $(DEV_DIR)/db
	ln -sf ../$(BIN) $(DEV_DIR)/$(BIN)
	ln -sf ../icons $(DEV_DIR)/icons
	ln -sf ../notes.ini.defaults $(DEV_DIR)/notes.ini.defaults

# dev-check: refuse to touch anything unless the sandbox ini really names
# the sandbox database — an empty or foreign db_dir would send the seed and
# the run to the default location (or a real database) instead.
dev-check: $(DEV_DIR)/notes.ini
	@grep -q '^db_dir=$(CURDIR)/$(DEV_DIR)/db$$' $(DEV_DIR)/notes.ini || { \
	  echo "$(DEV_DIR)/notes.ini does not point at $(DEV_DIR)/db — refusing" \
	       "(rm $(DEV_DIR)/notes.ini to regenerate it)"; exit 1; }

# Both prerequisites are ORDER-ONLY: dev-check is phony, and a phony
# prerequisite on the left of the bar counts as always newer, so the seed
# re-ran (and appended its notes again) on every run-dev.
$(DEV_DB): | dev-check $(BIN)
	cd $(DEV_DIR) && ./$(BIN) folder add Work && ./$(BIN) folder add Home/Kitchen \
	  && printf 'Meeting notes\n\n! Send the agenda due 2026-10-01\n! Book the room\n#work' | ./$(BIN) note new --folder Work - \
	  && printf 'Project plan\n\nA plan with a #work tag and some **body** text.' | ./$(BIN) note new --folder Work - \
	  && printf 'Groceries\n\n! Milk\n! Bread #home' | ./$(BIN) note new --folder Home - \
	  && printf 'Loose note\n\nNot in any folder.' | ./$(BIN) note new - \
	  && ./$(BIN) note tag 1 work && ./$(BIN) note tag 2 work \
	  && ./$(BIN) note tag 3 home
	@echo "seeded $(DEV_DB)"

run-dev: $(BIN) $(DEV_DB) dev-check
	cd $(DEV_DIR) && ./$(BIN)

clean-dev:
	rm -rf $(DEV_DIR)

# ---------------------------------------------------------------------------
# Headless tests and the blob scan.  Both build against GLib and SQLite
# ONLY — no GTK on the line — which is what proves the block model
# (document.[ch], bnbf.[ch]) needs none.
#   make test              — the unit tests (GLib's g_test harness)
#   make bnbf-scan         — build/bnbf-scan: round-trips every note blob
#                            in a database COPY and reports what the
#                            loader normalized (see tools/bnbf-scan.c)
# ---------------------------------------------------------------------------
MODEL_SRCS   := src/document.c src/bnbf.c
MODEL_CFLAGS := -std=c11 -Wall -Wextra -g -Isrc \
                $(shell $(PKGCONF) --cflags glib-2.0)
MODEL_LIBS   := $(shell $(PKGCONF) --libs glib-2.0)

build/test_document: tests/test_document.c $(MODEL_SRCS) src/document.h src/bnbf.h Makefile
	@mkdir -p build
	$(CC) $(MODEL_CFLAGS) -o $@ tests/test_document.c $(MODEL_SRCS) $(MODEL_LIBS)

test: build/test_document
	./build/test_document

build/bnbf-scan: tools/bnbf-scan.c $(MODEL_SRCS) src/document.h src/bnbf.h Makefile
	@mkdir -p build
	$(CC) $(MODEL_CFLAGS) $(shell $(PKGCONF) --cflags sqlite3) -o $@ \
	  tools/bnbf-scan.c $(MODEL_SRCS) $(MODEL_LIBS) $(shell $(PKGCONF) --libs sqlite3)

bnbf-scan: build/bnbf-scan

# The UI probe: a note view driven from a script, rendered to a PNG
# (tests/ui_probe.c).  Links the app's objects minus main.o.
PROBE_OBJS := $(filter-out build/main.o,$(OBJS))
build/ui-probe: tests/ui_probe.c $(PROBE_OBJS)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/ui_probe.c $(PROBE_OBJS) $(LDFLAGS)

ui-probe: build/ui-probe

# ui-test: every script in tests/ui/ through the probe, on a copy of the
# sandbox database (the #tag choices come from it).  Needs a display.
ui-test: build/ui-probe $(DEV_DB)
	@cp $(DEV_DB) build/ui-test.db
	@rc=0; for s in tests/ui/*.txt; do \
	  n=$$(basename $$s .txt); \
	  if (cd tests/ui && ../../build/ui-probe ../../build/ui-test.db $$n.txt ../../build/ui-$$n.png >/dev/null 2>../../build/ui-$$n.err); then \
	    echo "ok   $$s"; \
	  else echo "FAIL $$s"; grep -v "poll(2)" build/ui-$$n.err; rc=1; fi; \
	done; exit $$rc

# Remove all build artifacts.
clean:
	rm -rf build $(BIN) $(DIST)

# =============================================================================
# Optional packaging targets — everything lands in dist/.
# =============================================================================

DIST     := dist

# --- macOS .app bundle -------------------------------------------------------
# A minimal bundle around the binary: icons/ and the defaults ini sit next
# to the executable inside Contents/MacOS (the app resolves both relative
# to argv[0]).  icons/composition.png becomes the bundle icon via sips +
# iconutil.  The OUTPUT names are the app's, not the artwork's
# (notes.iconset → notes.icns, CFBundleIconFile "notes", and the Linux
# hicolor icon apps/notes.png the .desktop's Icon=notes points at), so
# swapping the source art is a one-line change and never touches the
# bundle layout or the desktop entry.
# The binary still links against the MacPorts GTK dylibs (absolute install
# names), so the bundle runs on this machine but is NOT self-contained.

APP_DIR  := $(DIST)/Notes.app
ICONSET  := $(DIST)/notes.iconset

app: $(BIN)
	@command -v iconutil >/dev/null || \
	  { echo "error: iconutil/sips not found — 'make app' is macOS-only"; \
	    exit 1; }
	rm -rf "$(APP_DIR)" "$(ICONSET)"
	mkdir -p "$(APP_DIR)/Contents/MacOS" "$(APP_DIR)/Contents/Resources" \
	         "$(ICONSET)"
	# The executable is named "Notes": for NIB-less apps (GTK builds
	# the menubar programmatically) macOS titles the app menu with the
	# PROCESS name, not CFBundleName — the binary's filename is the
	# only lever.  argv[0]-relative lookups (icons,
	# ini) resolve by directory, so the rename is harmless.
	cp $(BIN) "$(APP_DIR)/Contents/MacOS/Notes"
	cp -R icons "$(APP_DIR)/Contents/MacOS/icons"
	cp notes.ini.defaults "$(APP_DIR)/Contents/MacOS/"
	find "$(APP_DIR)" -name .DS_Store -delete
	for sz in 16 32 128 256 512; do \
	  sips -z $$sz $$sz icons/composition.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}.png" >/dev/null; \
	  dbl=$$((sz * 2)); \
	  sips -z $$dbl $$dbl icons/composition.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}@2x.png" >/dev/null; \
	done
	iconutil -c icns -o "$(APP_DIR)/Contents/Resources/notes.icns" \
	         "$(ICONSET)"
	rm -rf "$(ICONSET)"
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>CFBundleName</key><string>Notes</string>' \
	  '  <key>CFBundleDisplayName</key><string>Notes</string>' \
	  '  <key>CFBundleIdentifier</key><string>org.example.notes</string>' \
	  '  <key>CFBundleExecutable</key><string>Notes</string>' \
	  '  <key>CFBundleIconFile</key><string>notes</string>' \
	  '  <key>CFBundlePackageType</key><string>APPL</string>' \
	  '  <key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '  <key>CFBundleVersion</key><string>$(VERSION)</string>' \
	  '  <key>NSHighResolutionCapable</key><true/>' \
	  '</dict>' \
	  '</plist>' \
	  > "$(APP_DIR)/Contents/Info.plist"
	@echo "built $(APP_DIR)"

# --- shared Linux package staging ---------------------------------------------
# Both deb and rpm install the whole app to /opt/notes (the binary
# resolves icons/ and its defaults ini relative to argv[0]) and put a
# wrapper script on PATH that execs it by absolute path so that
# resolution works.  Per-user settings fall back to ~/.config/notes/
# because /opt is not writable (see on_app_config_init).

PKGROOT  := $(DIST)/pkgroot

pkgroot: $(BIN)
	rm -rf $(PKGROOT)
	mkdir -p $(PKGROOT)/opt/notes $(PKGROOT)/usr/bin \
	         $(PKGROOT)/usr/share/applications \
	         $(PKGROOT)/usr/share/icons/hicolor/512x512/apps
	cp $(BIN) $(PKGROOT)/opt/notes/
	cp -R icons $(PKGROOT)/opt/notes/icons
	cp notes.ini.defaults $(PKGROOT)/opt/notes/
	find $(PKGROOT) -name .DS_Store -delete
	printf '%s\n' \
	  '#!/bin/sh' \
	  '# Notes finds icons/ and its defaults ini next to argv[0];' \
	  '# exec by absolute path so both resolve into /opt/notes.' \
	  'exec /opt/notes/notes "$$@"' \
	  > $(PKGROOT)/usr/bin/notes
	chmod 755 $(PKGROOT)/usr/bin/notes
	printf '%s\n' \
	  '[Desktop Entry]' \
	  'Type=Application' \
	  'Name=Notes' \
	  'Comment=Notes with folders, tags and rich text' \
	  'Exec=/usr/bin/notes' \
	  'Icon=notes' \
	  'Terminal=false' \
	  'Categories=Utility;Office;' \
	  > $(PKGROOT)/usr/share/applications/notes.desktop
	cp icons/composition.png \
	   $(PKGROOT)/usr/share/icons/hicolor/512x512/apps/notes.png

# --- Debian package ------------------------------------------------------------
DEB_ARCH := $(shell dpkg --print-architecture 2>/dev/null || \
                    uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/')
DEB_ROOT := $(DIST)/deb-root

deb: pkgroot
	@command -v dpkg-deb >/dev/null || \
	  { echo "error: dpkg-deb not found — build .deb on a Debian/Ubuntu machine"; \
	    exit 1; }
	rm -rf $(DEB_ROOT)
	cp -R $(PKGROOT) $(DEB_ROOT)
	mkdir -p $(DEB_ROOT)/DEBIAN
	printf '%s\n' \
	  'Package: notes' \
	  'Version: $(VERSION)' \
	  'Section: editors' \
	  'Priority: optional' \
	  'Architecture: $(DEB_ARCH)' \
	  'Depends: libgtk-4-1, libsqlite3-0' \
	  'Maintainer: Ian Campbell <ian@camb.io>' \
	  'Description: Notes app with folders, tags and rich text' \
	  ' Apple Notes-style desktop notes application (GTK4 + SQLite).' \
	  > $(DEB_ROOT)/DEBIAN/control
	dpkg-deb --build --root-owner-group $(DEB_ROOT) \
	  $(DIST)/notes_$(VERSION)_$(DEB_ARCH).deb

# --- RPM package ----------------------------------------------------------------
RPM_ARCH := $(shell uname -m)

rpm: pkgroot
	@command -v rpmbuild >/dev/null || \
	  { echo "error: rpmbuild not found — build .rpm on a Fedora/RHEL machine"; \
	    exit 1; }
	rm -rf $(DIST)/rpm
	mkdir -p $(DIST)/rpm/SPECS
	printf '%s\n' \
	  'Name: notes' \
	  'Version: $(VERSION)' \
	  'Release: 1' \
	  'Summary: Notes app with folders, tags and rich text' \
	  'License: BSD-3-Clause' \
	  '%description' \
	  'Apple Notes-style desktop notes application (GTK4 + SQLite).' \
	  '%install' \
	  'cp -a $(abspath $(PKGROOT))/. %{buildroot}/' \
	  '%files' \
	  '/opt/notes' \
	  '/usr/bin/notes' \
	  '/usr/share/applications/notes.desktop' \
	  '/usr/share/icons/hicolor/512x512/apps/notes.png' \
	  > $(DIST)/rpm/SPECS/notes.spec
	rpmbuild -bb --define "_topdir $(abspath $(DIST))/rpm" \
	  $(DIST)/rpm/SPECS/notes.spec
	cp $(DIST)/rpm/RPMS/*/notes-$(VERSION)-1.*.rpm $(DIST)/

.PHONY: all run run-dev dev-check clean clean-dev test bnbf-scan ui-probe ui-test app pkgroot deb rpm
