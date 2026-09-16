# GTK4 migration Phase 0 spike.  Needs gtk4 via pkg-config (MacPorts:
# `sudo port install gtk4 +quartz`; Debian: `libgtk-4-dev`).

PKGCONF := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)
CFLAGS  := -std=c11 -Wall -Wextra -g $(shell $(PKGCONF) --cflags gtk4)
LDFLAGS := $(shell $(PKGCONF) --libs gtk4)

spike: spike.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: spike
	./spike

clean:
	rm -f spike

install:
	@echo "nothing to install — this is a throwaway spike"

.PHONY: run clean install
