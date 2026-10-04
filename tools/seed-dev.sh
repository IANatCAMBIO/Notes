#!/bin/bash
# =============================================================================
# seed-dev.sh — build a fresh, realistic development database
#
#   tools/seed-dev.sh DATA_DIR NOTES_CLI DEV_SEED IMAGE
#
# Called by `make seed`.  Everything goes through the `notes` CLI except the
# one note that shows every block kind, which build/dev_seed writes through
# the document model.  The result: nested folders (some with emoji), tags,
# pinned notes, trashed notes and a trashed folder, notes with images, a
# long note, forty filler notes with spread-out dates, and '!' action items
# — open, done, overdue, due today, due later, undated — in several
# contexts (a context is the folder the note sits in).
#
# DATA_DIR is wiped first.  It must be a folder directly inside this
# repository's dev/ directory: this script never touches a real data
# directory.
# =============================================================================
set -euo pipefail

DATA_DIR="$1"; NOTES="$2"; DEV_SEED="$3"; IMAGE="$4"

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

# note TITLE-AND-BODY [FOLDER] — create a note, print its id.
note() {
    if [ $# -ge 2 ]; then
        printf '%s' "$1" | "$NOTES" note new --folder "$2" - | cut -f1 | cut -d' ' -f2
    else
        printf '%s' "$1" | "$NOTES" note new - | cut -f1 | cut -d' ' -f2
    fi
}
day() { date -v"$1"d +%F; }          # day +3 / day -2: a date relative to today

# tag NOTE NAME... — give a note #tags (the CLI appends each as a tag token).
tag() {
    local id="$1"; shift
    for name in "$@"; do "$NOTES" note tag "$id" "$name" >/dev/null; done
}

"$NOTES" folder add "Salt/Deployments/Stride"  >/dev/null
"$NOTES" folder add "Salt/Deployments/Apollo"  >/dev/null
"$NOTES" folder add "Salt/Meetings"            >/dev/null
"$NOTES" folder add "Home/Kitchen"             >/dev/null
"$NOTES" folder add "Home/Garden"              >/dev/null
"$NOTES" folder add "Reading"                  >/dev/null
"$NOTES" folder add "Old Projects/2024"        >/dev/null
"$NOTES" folder emoji "Salt" "🧂"              >/dev/null
"$NOTES" folder emoji "Home" "🏡"              >/dev/null
"$NOTES" folder emoji "Reading" "📚"           >/dev/null
"$NOTES" folder ai-mode "Salt/Deployments" project >/dev/null

rich=$(note "Every block kind

This note shows each thing the editor draws." "Reading")
"$DEV_SEED" rich "$rich" "$IMAGE" >/dev/null

stride=$(note "Stride rollout plan

Sensor rollout for the Stride tenant.
! Rerun the traffic reports due $(day -3)
! Confirm the collector version due $(day +0)
! Send the summary to Priya due $(day +5)
! Archive the old dashboards
Notes from the kickoff call." "Salt/Deployments/Stride")
"$NOTES" action done "$stride:3" >/dev/null
tag "$stride" salt urgent

apollo=$(note "Apollo PoV checklist

! Book the validation call due $(day +2)
! Collect sample traffic due $(day +9)
Kong plugin deployed in staging." "Salt/Deployments/Apollo")
tag "$apollo" salt

meeting=$(note "Weekly sync

Attendees: Ian, Priya, Sam.
! Circulate the minutes due $(day +1)
Decisions: ship the hybrid upgrade next sprint." "Salt/Meetings")
"$NOTES" action done "$meeting:0" >/dev/null
tag "$meeting" salt meetings

groceries=$(note "Groceries

! Milk
! Bread due $(day +0)
Olive oil, tomatoes, basil." "Home/Kitchen")
tag "$groceries" home

garden=$(note "Garden jobs

! Prune the apple tree due $(day -10)
Order compost before the frost." "Home/Garden")
tag "$garden" home

loose=$(note "Loose note

Not in any folder.
! A root action item due $(day +14)")

pictures=$(note "Trip photos

Two pictures appended from files." "Home")
"$NOTES" note add-image "$pictures" "$IMAGE" >/dev/null
"$NOTES" note add-image "$pictures" "$(dirname "$IMAGE")/folder.png" >/dev/null

long=$(note "A long note

$(for i in $(seq 1 200); do echo "Line $i of a note long enough to scroll."; done)" "Reading")

"$NOTES" note pin "$stride" "$groceries" >/dev/null

# Forty filler notes with modified times a day apart, for list scrolling
# and sorting.
now=$(date +%s)
for i in $(seq 1 40); do
    id=$(note "Reading log $i

Chapter $i notes." "Reading")
    if [ $((i % 4)) = 0 ]; then tag "$id" reading; fi
    "$NOTES" note set-modified "$id" $((now - i * 86400)) >/dev/null
done

# The Trash: two notes, and a folder with a note inside it.
gone1=$(note "Draft to throw away

Nothing important.")
gone2=$(note "Old shopping list

! Something that no longer matters" "Home/Kitchen")
"$NOTES" note delete "$gone1" "$gone2" >/dev/null
note "Retired plan

From 2024." "Old Projects/2024" >/dev/null
"$NOTES" folder delete "Old Projects" >/dev/null

"$NOTES" stats
echo "seeded $DATA_DIR"
