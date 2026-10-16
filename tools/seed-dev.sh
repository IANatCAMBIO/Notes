#!/bin/bash
# =============================================================================
# seed-dev.sh — build a fresh, realistic development database
#
#   tools/seed-dev.sh DATA_DIR SHARED_DB DEV_SEED IMAGE IMAGE2
#
# Called by `make seed`.  build/dev_seed writes everything through the core
# API (tools/dev_seed.c says what): tags in notes' Tags fields and #tags in
# their text (some with emoji, two favorites), pinned notes, trashed notes,
# a note with every block kind, notes with images, a long note, forty filler
# notes with spread-out dates, and '!' action items — open, done, overdue,
# due today, due later, undated — in notes of several tags.
#
# DATA_DIR is wiped first.  It must be a folder directly inside this
# repository's dev/ directory and SHARED_DB (the shared database file,
# CAMBIO_SHARED_DB) a file directly inside DATA_DIR; no app may be running
# on it (dev_seed check refuses while one holds the data directory's lock).
# This script never touches a real data directory or database.
# =============================================================================
set -euo pipefail

DATA_DIR="$1"; SHARED_DB="$2"; DEV_SEED="$3"; IMAGE="$4"; IMAGE2="$5"

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
if [ "$(dirname "$DATA_DIR")" != "$ROOT/dev" ] || [ "$(basename "$DATA_DIR")" = ".." ]; then
    echo "seed-dev: refusing: $DATA_DIR is not a folder directly inside $ROOT/dev" >&2
    exit 2
fi
if [ "$(dirname "$SHARED_DB")" != "$DATA_DIR" ]; then
    echo "seed-dev: refusing: $SHARED_DB is not a file directly inside $DATA_DIR" >&2
    exit 2
fi
export NOTES_DATA_DIR="$DATA_DIR"
export CAMBIO_SHARED_DB="$SHARED_DB"

mkdir -p "$DATA_DIR"
"$DEV_SEED" check
rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"
"$DEV_SEED" seed "$IMAGE" "$IMAGE2"
echo "seeded $DATA_DIR"
