# =============================================================================
# Notes — Makefile
#
#   make            build Notes.app
#   make run        run the app on the development data in dev/data
#   make seed       (re)create the development database in dev/data
#   make test       the model, core and UI tests
#   make ui-check   the windows built in-process: snapshots in dev/snapshots,
#                   ownership and memory checks (part of make test)
#   make bnbf-scan  build/bnbf-scan, the blob round-trip scan (tools/)
#   make install    install the bundle in ~/Applications
#   make clean      remove build/ and the bundle (dev/ is kept)
#   make clean-dev  remove dev/ (development and test data)
#
# Requires the Xcode command-line tools, MacPorts glib2 + sqlite3 + pkgconf,
# and the shared_data repository beside this one (SHARED_DATA_DIR).
# See BUILD.md.
# =============================================================================

# The names of things live in src/identity.h and nowhere else: `ident` reads
# one #define out of it.
ident        = $(shell sed -n 's/^\#define $(1) *"\(.*\)"/\1/p' src/identity.h)
APP_NAME    := $(call ident,ON_APP_NAME)
BUNDLE_NAME := $(call ident,ON_BUNDLE_NAME)
BUNDLE_ID   := $(call ident,ON_BUNDLE_ID)
DATA_ENV    := $(call ident,ON_DATA_DIR_ENV)
ifeq ($(and $(APP_NAME),$(BUNDLE_NAME),$(BUNDLE_ID),$(DATA_ENV)),)
$(error src/identity.h does not define every name the Makefile reads)
endif

BUNDLE   = $(BUNDLE_NAME).app
CONTENTS = $(BUNDLE)/Contents
MACOS    = $(CONTENTS)/MacOS
RSRC     = $(CONTENTS)/Resources
APP_BIN  = $(MACOS)/$(BUNDLE_NAME)

CC       = clang

# The version shown in the About panel; edit VERSION to change it.
VERSION := $(strip $(shell cat VERSION))
ifeq ($(VERSION),)
$(error the VERSION file is missing or empty)
endif

# pkg-config: MacPorts installs it in /opt/local/bin, which is not on every
# shell's PATH.
PKGCONF := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# GLib (phase one keeps it in the core) and SQLite, both from MacPorts via
# pkg-config.  SQLite is MacPorts' on purpose: Tasks links the same build,
# so the two apps sharing one database file run one SQLite (WAL, file
# format, locking) rather than two different versions.
PKGS        = glib-2.0 sqlite3
PKG_CFLAGS := $(shell $(PKGCONF) --cflags $(PKGS))
PKG_LIBS   := $(shell $(PKGCONF) --libs $(PKGS))
ifeq ($(PKG_LIBS),)
$(error glib-2.0 or sqlite3 not found: sudo port install glib2 sqlite3 pkgconf)
endif
GLIB_CFLAGS := $(shell $(PKGCONF) --cflags glib-2.0)
GLIB_LIBS   := $(shell $(PKGCONF) --libs glib-2.0)
GLIB_DYLIB  := $(shell $(PKGCONF) --variable=libdir glib-2.0)/libglib-2.0.dylib

# ── The shared database module ───────────────────────────────────────────
# Notes and Tasks share one SQLite file; the module that owns its schema,
# its location, its connections and its backups lives in the shared_data
# repository beside this one and is compiled into the core.
SHARED_DATA_DIR ?= $(CURDIR)/../shared_data
SHARED_SRC       = $(SHARED_DATA_DIR)/src/shared_db.c
ifeq ($(wildcard $(SHARED_SRC)),)
$(error $(SHARED_SRC) not found: check out shared_data beside this repository or set SHARED_DATA_DIR)
endif

# Deployment target. The sources are COMPILED for MIN_MACOS, so using an API
# newer than that without an @available check is a build error today. The
# product is LINKED for the macOS version MacPorts built GLib for, because
# while GLib is a dependency that is the oldest system the binary can really
# run on (and the linker warns about claiming less). When GLib goes, the two
# become the same number.
MIN_MACOS   = 14.0
LINK_MACOS := $(shell otool -l $(GLIB_DYLIB) | awk '$$1 == "minos" { print $$2; exit }')

# Code-signing identity (as in Files). macOS ties privacy grants to the
# app's signature: for an ad-hoc ("-") signature that is the exact build, so
# every rebuild loses the grant; for a certificate it is the bundle ID plus
# certificate, which rebuilds keep. Defaults to the first "Apple
# Development" identity in the keychain, else ad hoc. Override with
#   make CODESIGN_IDENTITY="…"   (list: security find-identity -v -p codesigning)
#
# Only goals that build the bundle look the identity up.
SIGNING_GOALS := $(if $(MAKECMDGOALS),$(filter-out clean clean-dev bnbf-scan,$(MAKECMDGOALS)),all)
ifneq ($(SIGNING_GOALS),)
ifeq ($(origin CODESIGN_IDENTITY),undefined)
CODESIGN_IDENTITY := $(or $(shell security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -n 1),-)
endif

# build/codesign.id records the signing identity. When the identity changes
# it is rewritten (while the Makefile is read) and the bundle is backdated,
# which forces a re-sign; an unchanged identity triggers nothing.
$(shell mkdir -p build && (echo '$(CODESIGN_IDENTITY)' | cmp -s - build/codesign.id \
    || { echo '$(CODESIGN_IDENTITY)' > build/codesign.id; \
         [ -d $(BUNDLE) ] && touch -t 200001010000 $(BUNDLE); true; }))
endif

# sign: codesign one thing, saying nothing unless it fails (a rebuild would
# otherwise report "replacing existing signature" every time).
sign = codesign --force --sign "$(CODESIGN_IDENTITY)" $(1) 2>build/codesign.log \
       || { cat build/codesign.log >&2; exit 1; }

# -Werror=unguarded-availability: calling an API newer than the deployment
# target without an @available check is a build error. (Clang cannot check
# messages to __kindof receivers such as NSApp.)
# -MMD -MP: emit build/*.d header-dependency files so editing a header
# rebuilds every object that includes it.
# -Wno-unused-parameter, Objective-C only: the parameters left unused are
# ones AppKit's delegate and action signatures require. The C core keeps the
# warning.
COMMON   = -O2 -Wall -Wextra -Werror=unguarded-availability \
           -mmacosx-version-min=$(MIN_MACOS) -I src -I $(SHARED_DATA_DIR)/src \
           -MMD -MP -DON_VERSION='"$(VERSION)"' $(PKG_CFLAGS)
CFLAGS   = -std=c11 $(COMMON)
MFLAGS   = -fobjc-arc $(COMMON) -Wno-unused-parameter

# What the core links: GLib, MacPorts' SQLite, and ImageIO for image_io.c.
LDFLAGS   = -mmacosx-version-min=$(LINK_MACOS)
CORE_LIBS = $(PKG_LIBS) \
            -framework CoreFoundation -framework CoreGraphics -framework ImageIO
APP_LIBS  = $(CORE_LIBS) -framework Cocoa -framework CoreText \
            -framework UniformTypeIdentifiers

# The core: no UI, shared by the app, the development seeder and the tests.
CORE_SRCS = src/action_edit.c src/app_core.c src/app_lifecycle.c \
            src/backup.c src/bnbf.c src/config.c src/db.c \
            src/document.c src/export.c src/image_io.c \
            src/main_service.c src/search_job.c src/search_query.c \
            src/serialize.c src/utf_index.c
# Everything else in src/ is the app: its C (the editor's geometry and
# editing policy) and its Objective-C.
UI_C_SRCS = $(filter-out $(CORE_SRCS),$(wildcard src/*.c))
UI_M_SRCS = $(wildcard src/*.m)

CORE_OBJS = $(patsubst src/%.c,build/%.o,$(CORE_SRCS)) build/shared_db.o
UI_OBJS   = $(patsubst src/%.c,build/%.o,$(UI_C_SRCS)) \
            $(patsubst src/%.m,build/%.o,$(UI_M_SRCS))
DEPS      = $(CORE_OBJS:.o=.d) $(UI_OBJS:.o=.d)

.PHONY: all run seed test ui-check bnbf-scan install clean clean-dev

all: $(BUNDLE)

# Info.plist gets every name from src/identity.h, the version from VERSION
# and the time the bundle was assembled as NotesBuildDate (UTC), shown in
# the About panel. Every PNG in icons/ is copied into Contents/Resources,
# where the app loads it by name, and README.md, which Help opens.
IMAGES   = $(wildcard icons/*.png)

$(BUNDLE): $(APP_BIN) resources/Info.plist VERSION src/identity.h $(IMAGES) README.md
	@rm -rf $(RSRC) && mkdir -p $(RSRC)  # fresh, so removed images do not linger
	@cp resources/Info.plist $(CONTENTS)/Info.plist
	@plutil -replace CFBundleName -string "$(APP_NAME)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleDisplayName -string "$(APP_NAME)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleIdentifier -string "$(BUNDLE_ID)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleExecutable -string "$(BUNDLE_NAME)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleShortVersionString -string "$(VERSION)" $(CONTENTS)/Info.plist
	@plutil -replace CFBundleVersion -string "$(VERSION)" $(CONTENTS)/Info.plist
	@plutil -replace LSMinimumSystemVersion -string "$(LINK_MACOS)" $(CONTENTS)/Info.plist
	@plutil -replace NotesBuildDate -string "$$(date -u +%Y-%m-%dT%H:%M:%SZ)" $(CONTENTS)/Info.plist
	@cp $(IMAGES) README.md $(RSRC)/
	@$(call sign,$(BUNDLE))
	@touch $(BUNDLE)  # codesign rewrites the binaries; keep the bundle newer

# The app executable is linked WITHOUT the linker's own ad-hoc signature:
# the bundle rule signs it, and codesign would otherwise report "replacing
# existing signature" on every build.
$(APP_BIN): $(CORE_OBJS) $(UI_OBJS)
	@mkdir -p $(MACOS)
	$(CC) $(LDFLAGS) -Wl,-no_adhoc_codesign -o $@ $^ $(APP_LIBS)

# Objects also depend on the Makefile so flag changes rebuild everything,
# and on VERSION because ON_VERSION is compiled in.
build/%.o: src/%.c Makefile VERSION | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.o: src/%.m Makefile VERSION | build
	$(CC) $(MFLAGS) -c -o $@ $<

# The shared module, compiled with the core's flags.
build/shared_db.o: $(SHARED_SRC) Makefile | build
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(DEPS)

build:
	@mkdir -p build

# ── Development data ────────────────────────────────────────────────────────
# Every run and test from this tree sets NOTES_DATA_DIR, so the ini, the
# database and the lock all live under dev/ (gitignored) and the user's
# real data directory is never touched. DEV_DATA is the one a person looks
# at (`make run`); the tests use their own folders under dev/test.
DEV_DATA = $(CURDIR)/dev/data
override DEV_SHARED_DB = $(DEV_DATA)/shared_data.db
DEV_ENV  = $(DATA_ENV)="$(DEV_DATA)" CAMBIO_SHARED_DB="$(DEV_SHARED_DB)"

build/dev_seed.o: tools/dev_seed.c Makefile | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/dev_seed: build/dev_seed.o $(CORE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(CORE_LIBS)

-include build/dev_seed.d

# seed: a fresh, realistic database — nested folders, tags, pinned and
# trashed notes, every block kind, action items in several contexts —
# written through the core API by build/dev_seed. It refuses to run while
# an app holds dev/data.
seed: build/dev_seed
	@tools/seed-dev.sh "$(DEV_DATA)" "$(DEV_SHARED_DB)" build/dev_seed icons/composition.png tests/fixtures/folder.png

$(DEV_DATA)/notes.db:
	@$(MAKE) seed

# run: the app in the foreground on the development data, its log on this
# terminal. Quit it with ⌘Q.
run: $(BUNDLE) $(DEV_DATA)/notes.db
	@echo "$(DATA_ENV)=$(DEV_DATA)"
	@$(DEV_ENV) $(APP_BIN)

# ── Tests ───────────────────────────────────────────────────────────────────
# The model tests link document.c and bnbf.c against GLib ALONE — which is
# what proves the block model needs nothing else. Every other *_test.c
# links the whole core. Each test gets its own data directory under dev/test.
TEST_DATA  = $(CURDIR)/dev/test
TEST_SRCS  = $(wildcard tests/*_test.c)
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_SRCS))

build/test_document: tests/test_document.c src/document.c src/bnbf.c \
                     src/document.h src/bnbf.h Makefile | build
	$(CC) -std=c11 -O2 -Wall -Wextra -I src $(GLIB_CFLAGS) $(LDFLAGS) -o $@ \
	    tests/test_document.c src/document.c src/bnbf.c $(GLIB_LIBS)

build/%_test.o: tests/%_test.c Makefile | build
	$(CC) $(CFLAGS) -I tests -c -o $@ $<

build/%_test: build/%_test.o $(CORE_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(CORE_LIBS)

# UI tests (tests/*_test.m) link every app object except main.o, and run
# from a bundle of their own that borrows the app's Resources, so
# NSBundle.mainBundle finds the icons.
UI_TEST_SRCS = $(wildcard tests/*_test.m)
UI_TEST_APPS = $(patsubst tests/%.m,build/%.app,$(UI_TEST_SRCS))
APP_OBJS     = $(CORE_OBJS) $(filter-out build/main.o,$(UI_OBJS))

build/%_test.o: tests/%_test.m Makefile | build
	$(CC) $(MFLAGS) -I tests -c -o $@ $<

build/%_test.app: build/%_test.o $(APP_OBJS) $(BUNDLE)
	@rm -rf $@ && mkdir -p $@/Contents/MacOS
	$(CC) $(LDFLAGS) -o $@/Contents/MacOS/$*_test $< $(APP_OBJS) $(APP_LIBS)
	@ln -s $(CURDIR)/$(RSRC) $@/Contents/Resources

.PRECIOUS: build/%_test.o
-include $(TEST_BINS:=.d) $(UI_TEST_APPS:.app=.d)

test: build/test_document $(TEST_BINS) $(UI_TEST_APPS) $(BUNDLE)
	@echo "== build/test_document"; ./build/test_document
	@for t in $(TEST_BINS) $(UI_TEST_APPS); do \
	    name=$$(basename $$t .app); echo "== $$t"; \
	    case $$t in *.app) run=$$t/Contents/MacOS/$$name ;; *) run=$$t ;; esac; \
	    rm -rf "$(TEST_DATA)/$$name" && mkdir -p "$(TEST_DATA)/$$name" && \
	    $(DATA_ENV)="$(TEST_DATA)/$$name" \
	    CAMBIO_SHARED_DB="$(TEST_DATA)/$$name/shared_data.db" ./$$run || exit 1; \
	done

# ui-check: the in-process UI test on its own (tests/ui_smoke_test.m): it
# builds the real windows on a test database, checks that closed windows
# are released, that open/close cycles do not grow the heap and that no
# full-size image is being held for a small view, and writes PNG snapshots
# of each window, light and dark, to dev/snapshots.  It needs no Screen
# Recording, Accessibility or Developer Tools permission — which is why it
# exists: `leaks`, `vmmap` and `footprint` stop for an authorisation prompt
# on a Mac with developer mode off.
ui-check: build/ui_smoke_test.app
	@rm -rf "$(TEST_DATA)/ui_smoke_test" && mkdir -p "$(TEST_DATA)/ui_smoke_test"
	@$(DATA_ENV)="$(TEST_DATA)/ui_smoke_test" \
	    CAMBIO_SHARED_DB="$(TEST_DATA)/ui_smoke_test/shared_data.db" ./build/ui_smoke_test.app/Contents/MacOS/ui_smoke_test

# bnbf-scan: round-trips every note blob of a database COPY through the
# model and reports what the loader normalised (tools/bnbf-scan.c).
build/bnbf-scan: tools/bnbf-scan.c src/document.c src/bnbf.c src/document.h src/bnbf.h Makefile | build
	$(CC) -std=c11 -O2 -Wall -Wextra -I src $(PKG_CFLAGS) $(LDFLAGS) -o $@ \
	    tools/bnbf-scan.c src/document.c src/bnbf.c $(PKG_LIBS)

bnbf-scan: build/bnbf-scan

# ── Install ─────────────────────────────────────────────────────────────────
# Per-user install: no sudo needed. The old bundle is removed first because
# `cp -R` into an existing bundle directory would nest the new one inside it.
#
# A running installed copy is asked to quit first (SIGTERM, which the app
# turns into an ordinary Quit: editors flush, see app_lifecycle.h) and is
# reopened afterwards. Install stops if it has not exited after 5 s. The
# data directory (~/Library/Application Support/Notes) is outside the
# bundle, so replacing the bundle never touches settings or notes.
INSTALL_DIR   = $(HOME)/Applications
INSTALLED_BIN = $(INSTALL_DIR)/$(BUNDLE)/Contents/MacOS/$(BUNDLE_NAME)

install: all
	@mkdir -p "$(INSTALL_DIR)"
	@running=0; \
	if pgrep -qxf "$(INSTALLED_BIN)"; then \
	    running=1; \
	    pkill -TERM -xf "$(INSTALLED_BIN)"; \
	    for i in 1 2 3 4 5 6 7 8 9 10; do \
	        pgrep -qxf "$(INSTALLED_BIN)" || break; sleep 0.5; \
	    done; \
	    if pgrep -qxf "$(INSTALLED_BIN)"; then \
	        echo "$(APP_NAME) did not quit; quit it (⌘Q) and run make install again" >&2; \
	        exit 1; \
	    fi; \
	    echo "Quit the running $(APP_NAME)"; \
	fi; \
	rm -rf "$(INSTALL_DIR)/$(BUNDLE)" && \
	cp -R $(BUNDLE) "$(INSTALL_DIR)/$(BUNDLE)" && \
	echo "Installed to $(INSTALL_DIR)/$(BUNDLE)" && \
	if [ $$running = 1 ]; then open "$(INSTALL_DIR)/$(BUNDLE)" && echo "Reopened $(APP_NAME)"; fi

clean:
	@rm -rf build $(BUNDLE)

clean-dev:
	@rm -rf dev
