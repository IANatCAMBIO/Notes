#!/bin/bash
# =============================================================================
# seed-dev.sh — build a fresh, realistic development database
#
#   tools/seed-dev.sh DATA_DIR DEV_SEED IMAGE IMAGE2
#
# Called by `make seed`.  build/dev_seed writes everything through the core
# API (tools/dev_seed.c says what): nested folders (some with emoji), tags,
# pinned notes, trashed notes and a trashed folder, a note with every block
# kind, notes with images, a long note, forty filler notes with spread-out
# dates, and '!' action items — open, done, overdue, due today, due later,
# undated — in several contexts (a context is the folder the note sits in).
#
# DATA_DIR is wiped first.  It must be a folder directly inside this
# repository's dev/ directory, and no app may be running on it (dev_seed
# check refuses while one holds the data directory's lock): this script
# never touches a real data directory.
# =============================================================================
set -euo pipefail

DATA_DIR="$1"; DEV_SEED="$2"; IMAGE="$3"; IMAGE2="$4"

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
if [ "$(dirname "$DATA_DIR")" != "$ROOT/dev" ] || [ "$(basename "$DATA_DIR")" = ".." ]; then
    echo "seed-dev: refusing: $DATA_DIR is not a folder directly inside $ROOT/dev" >&2
    exit 2
fi
export NOTES_DATA_DIR="$DATA_DIR"

mkdir -p "$DATA_DIR"
"$DEV_SEED" check
rm -rf "$DATA_DIR"
mkdir -p "$DATA_DIR"
"$DEV_SEED" seed "$IMAGE" "$IMAGE2"
echo "seeded $DATA_DIR"
