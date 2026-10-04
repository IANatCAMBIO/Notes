#!/bin/bash
# =============================================================================
# cli_golden.sh — this build's CLI against the GTK build's, byte for byte
#
#   tests/cli_golden.sh ROOT FORK_CLI DEV_SEED      (run by `make golden`)
#
# The CLI is a contract: a Claude skill and scripts drive it.  This test
# runs the same commands through this build's `notes` and through a GTK
# reference binary, on identical copies of the same databases, and compares
# stdout, stderr and the exit code of every one — plain and --json — plus
# the files that export, backup and `note image` write.
#
# Two databases are used: one seeded by this build (CLI + build/dev_seed,
# so it holds every block kind and images) and one seeded by the GTK build.
# Each is copied for the two readers, so each build also reads what the
# other wrote.  A sequence of mutations then runs on both copies and the
# results are compared again.
#
# THE GTK REFERENCE BINARY AND THE USER'S REAL DATA
#
# The GTK build resolves its ini next to its own binary and falls back to
# ~/.config/notes and ~/.local/share/notes; its socket lives in ~/.cache.
# The user's real database is reachable through those.  So the reference is
# handled like this, and the script stops if any step does not hold:
#   - it is built from this repository's own `main` branch, exported with
#     `git archive` into a scratch folder that is deleted as soon as the
#     binary is linked (no checkout of, or dependence on, the user's real
#     notes tree, whose notes.ini names the real database — and no GTK
#     Makefile left lying around, whose `install` target replaces the
#     user's installed app);
#   - the binary is kept at dev/gtk_ref/notes WITHOUT its execute bit, so
#     it cannot be run in place; it only ever runs as a COPY inside a
#     per-case folder under dev/golden that already holds a notes.ini
#     whose db_dir points into that same folder, and every executable copy
#     is deleted when this script exits, however it exits;
#   - it runs under `env -i` with HOME and every XDG directory redirected
#     into dev/golden/home, so even its fallbacks (default database, user
#     config, cache/runtime socket directory) resolve under dev/;
#   - before the first write in each folder, `stats` (which prints the
#     database path it opened) must name the expected file;
#   - at the end, dev/golden/home must still be empty of notes data, which
#     shows the fallbacks were never taken.
# Only commands known to cli.c are passed: an unknown first word would make
# the GTK binary start its GUI.
#
# Not compared, and why:
#   - `note add-image`: the two builds encode PNG with different libraries
#     (ImageIO here, gdk-pixbuf there), so the stored bytes differ in size;
#   - after the mutation sequence, modified/created TIMES: the two runs
#     happen seconds apart.  They are masked (and the modified times are
#     set equal in both databases first, so the ORDER of listings, which
#     follows them, still counts); everything else still counts.
# =============================================================================
set -uo pipefail

ROOT="$1"; FORK_CLI="$ROOT/$2"; DEV_SEED="$ROOT/$3"
G="$ROOT/dev/golden"
REF="$ROOT/dev/gtk_ref"
T="$G/tmp"

die() { echo "golden: $*" >&2; exit 1; }

case "$G" in */dev/golden) ;; *) die "refusing: $G is not under dev/" ;; esac

PKGCONF=$(command -v pkg-config || echo /opt/local/bin/pkg-config)
if ! "$PKGCONF" --exists gtk4 2>/dev/null; then
    echo "golden: SKIPPED — MacPorts gtk4 is not installed, so there is no GTK reference to compare with"
    exit 0
fi

# ── The reference binary ────────────────────────────────────────────────────
# Built once per commit of `main`: dev/gtk_ref/commit records which.
WANT=$(git -C "$ROOT" rev-parse main) || die "this repository has no main branch"
if [ ! -f "$REF/notes" ] || [ "$(cat "$REF/commit" 2>/dev/null)" != "$WANT" ]; then
    BUILD="$ROOT/dev/gtk_build"
    rm -rf "$BUILD" "$REF" && mkdir -p "$BUILD" "$REF"
    git -C "$ROOT" archive "$WANT" | tar -x -C "$BUILD" || die "cannot export main"
    if ! PATH="$(dirname "$PKGCONF"):$PATH" make -C "$BUILD" notes >"$ROOT/dev/gtk_build.log" 2>&1; then
        rm -rf "$BUILD"
        die "the GTK reference did not build (see dev/gtk_build.log)"
    fi
    install -m 0644 "$BUILD/notes" "$REF/notes"   # no execute bit: never run in place
    echo "$WANT" > "$REF/commit"
    rm -rf "$BUILD"
fi
[ ! -x "$REF/notes" ] || die "dev/gtk_ref/notes is executable; it must not be"

# However this script ends, leave no runnable GTK binary behind.
trap 'rm -f "$G"/gtk_*/notes' EXIT

rm -rf "$G"
mkdir -p "$G/home" "$T"

# gtk_case NAME — a folder the reference can run in: a copy of the binary
# beside an ini that names a database folder inside it.
gtk_case() {
    local dir="$G/gtk_$1"
    mkdir -p "$dir/db" "$dir/work"
    printf '[notes]\ndb_dir=%s\n' "$dir/db" > "$dir/notes.ini"
    cp "$REF/notes" "$dir/notes"
    chmod u+x "$dir/notes"
}
# fork_case NAME — the same for this build: just a data directory.
fork_case() { mkdir -p "$G/fork_$1/work"; }

# gtk NAME ARGS… / fork NAME ARGS… — run one CLI in its case folder, in a
# clean environment, from its work/ directory.
gtk() {
    local dir="$G/gtk_$1"; shift
    ( cd "$dir/work" && env -i PATH=/usr/bin:/bin HOME="$G/home" \
        XDG_DATA_HOME="$G/home/data" XDG_CONFIG_HOME="$G/home/config" \
        XDG_CACHE_HOME="$G/home/cache" XDG_RUNTIME_DIR="$G/home/run" \
        "$dir/notes" "$@" )
}
fork() {
    local dir="$G/fork_$1"; shift
    ( cd "$dir/work" && env -i PATH=/usr/bin:/bin HOME="$G/home" \
        NOTES_DATA_DIR="$dir" "$FORK_CLI" "$@" )
}

# gtk_confirm NAME — the reference must be on the database in its own
# folder before anything is written through it.
gtk_confirm() {
    local want="$G/gtk_$1/db/notes.db"
    local got
    got=$(gtk "$1" stats | sed -n 's/^database	//p')
    [ "$got" = "$want" ] || die "the GTK reference opened '$got', not '$want' — stopping before any write"
}

# ── Seeding (CLI only, so either build can run it) ──────────────────────────
# seed RUNNER NAME — the same content through either CLI.
seed() {
    local run="$1" name="$2"
    "$run" "$name" folder add "Salt/Deployments/Stride"  >/dev/null
    "$run" "$name" folder add "Salt/Meetings"            >/dev/null
    "$run" "$name" folder add "Home/Kitchen"             >/dev/null
    "$run" "$name" folder add "Old/2024"                 >/dev/null
    "$run" "$name" folder emoji "Salt" "🧂"              >/dev/null
    "$run" "$name" folder ai-mode "Salt" project         >/dev/null
    printf 'Stride rollout\n\nSensor rollout.\n! Rerun the reports due 2026-09-30\n! Confirm the version due 10/3/26\n! Archive dashboards\nTabs\there and "quotes" and a \\ backslash.' \
        | "$run" "$name" note new --folder "Salt/Deployments/Stride" - >/dev/null
    printf 'Weekly sync\n\n! Circulate the minutes due 2026-10-04\nDecisions: ship it.' \
        | "$run" "$name" note new --folder "Salt/Meetings" - >/dev/null
    printf 'Groceries\n\n! Milk\n! Bread due 2026-10-03\nOlive oil, tomatoes.' \
        | "$run" "$name" note new --folder "Home/Kitchen" - >/dev/null
    "$run" "$name" note new "Loose note" >/dev/null
    printf 'Unicode caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x93\x93\n\nsend due diligence report due 12/31/26\n! due 2026-01-01\n!\n! Real one' \
        | "$run" "$name" note new - >/dev/null
    printf 'Throwaway\n\nnothing' | "$run" "$name" note new - >/dev/null
    printf 'Retired plan\n\nold' | "$run" "$name" note new --folder "Old/2024" - >/dev/null
    "$run" "$name" note tag 1 salt      >/dev/null
    "$run" "$name" note tag 1 urgent    >/dev/null
    "$run" "$name" note tag 2 salt      >/dev/null
    "$run" "$name" note tag 3 home      >/dev/null
    "$run" "$name" note pin 1 3         >/dev/null
    "$run" "$name" action done 1:2      >/dev/null
    "$run" "$name" note delete 6        >/dev/null
    "$run" "$name" folder delete "Old"  >/dev/null
    for id in 1 2 3 4 5 6 7; do
        "$run" "$name" note set-modified "$id" $((1790000000 + id * 3600)) >/dev/null
    done
}

# ── Comparing ───────────────────────────────────────────────────────────────
passed=0; failed=0
MASK_TIMES=0                         # 1 after the mutations (see the header)

# normalise FILE DATA-PATH — make the two builds' outputs comparable: the
# database folder becomes <DATA>, and, once MASK_TIMES is set, times do too.
normalise() {
    sed -e "s|$2|<DATA>|g" "$1" > "$1.n"
    if [ "$MASK_TIMES" = 1 ]; then
        sed -E -i '' \
            -e 's/[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}/<TIME>/g' \
            -e 's/"(modified|created)_at":[0-9]+/"\1_at":<T>/g' "$1.n"
    fi
}

# same CASE [--stdin TEXT] ARGS… — run one command through both builds and
# compare stdout, stderr and exit code.
same() {
    local name="$1"; shift
    local input=""
    if [ "$1" = "--stdin" ]; then input="$2"; shift 2; fi
    printf '%s' "$input" | fork "$name" "$@" >"$T/f.out" 2>"$T/f.err"; local frc=$?
    printf '%s' "$input" | gtk  "$name" "$@" >"$T/g.out" 2>"$T/g.err"; local grc=$?
    normalise "$T/f.out" "$G/fork_$name"; normalise "$T/f.err" "$G/fork_$name"
    normalise "$T/g.out" "$G/gtk_$name/db"; normalise "$T/g.err" "$G/gtk_$name/db"
    if [ "$frc" = "$grc" ] && cmp -s "$T/f.out.n" "$T/g.out.n" && cmp -s "$T/f.err.n" "$T/g.err.n"; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL [$name] notes $*   (exit: this build $frc, GTK $grc)"
        diff "$T/g.out.n" "$T/f.out.n" | head -20
        diff "$T/g.err.n" "$T/f.err.n" | head -10
    fi
}

# same_tree CASE PATH — a file or folder both builds wrote under work/.
same_tree() {
    if diff -r "$G/fork_$1/work/$2" "$G/gtk_$1/work/$2" >"$T/tree.diff" 2>&1; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL [$1] files written to $2 differ"
        head -20 "$T/tree.diff"
    fi
}

# reads CASE — every read-only command, plain and --json.
reads() {
    local c="$1"
    same "$c" help
    same "$c" stats;                         same "$c" stats --json
    same "$c" tag list;                      same "$c" tag list --json
    same "$c" tag notes salt;                same "$c" tag notes salt --json
    same "$c" tag notes nosuchtag
    same "$c" folder list;                   same "$c" folder list --json
    same "$c" folder info Salt;              same "$c" folder info Salt --json
    same "$c" folder info Salt/Deployments/Stride --json
    same "$c" folder info Nope
    same "$c" note list;                     same "$c" note list --json
    same "$c" note list Salt/Meetings;       same "$c" note list Salt/Meetings --json
    same "$c" note list --all;               same "$c" note list --all --json
    same "$c" note list --recent;            same "$c" note list --recent --json
    same "$c" note list --pinned;            same "$c" note list --pinned --json
    same "$c" note list Nope
    for id in 1 2 3 4 5 6 7 8 9; do
        same "$c" note info "$id";           same "$c" note info "$id" --json
        same "$c" note cat "$id";            same "$c" note cat "$id" --json
        same "$c" note cat "$id" --md;       same "$c" note cat "$id" --md --json
        same "$c" note tags "$id";           same "$c" note tags "$id" --json
        same "$c" note images "$id";         same "$c" note images "$id" --json
    done
    same "$c" action list;                   same "$c" action list --json
    same "$c" action list --open;            same "$c" action list --done --json
    same "$c" action list --uid;             same "$c" action list --uid --json
    same "$c" action list --context Stride;  same "$c" action list --context stride --json
    same "$c" action list --context "";      same "$c" action list --context Nope
    same "$c" action list --bogus
    same "$c" action show 1;                 same "$c" action show 1 --json
    same "$c" action show 1:1;               same "$c" action show 1:1 --json
    same "$c" action show 999;               same "$c" action show 9:9
    same "$c" search rollout;                same "$c" search rollout --json
    same "$c" search "sensor rollout";       same "$c" search '"Sensor rollout"'
    same "$c" search "rollout -sensor";      same "$c" search "-rollout" --json
    same "$c" search 'r[eo]+ll' --regex;     same "$c" search '(' --regex
    same "$c" search "CAFÉ";                 same "$c" search ""
    same "$c" search nothingmatchesthis --json
    same "$c" trash list;                    same "$c" trash list --json
    same "$c" trash empty
    same "$c" note
    same "$c" note cat
    same "$c" note cat notanumber
    same "$c" folder
    same "$c" tag frobnicate
    same "$c" export-md out_md;              same_tree "$c" out_md
    same "$c" export-html out_html;          same_tree "$c" out_html
    same "$c" note image 1 1 image1.png
    same "$c" note image 1 99 none.png
}

# mutations CASE — every command that writes, in one fixed order.
mutations() {
    local c="$1"
    same "$c" folder add "New/Nested/Deep"
    same "$c" folder add "New/Nested/Deep"
    same "$c" folder rename "New/Nested" "Renamed"
    same "$c" folder move "New/Renamed/Deep" /
    same "$c" folder move "Nope" /
    same "$c" folder emoji "New" "📁"
    same "$c" folder emoji "New" -
    same "$c" folder ai-mode "New" custom
    same "$c" folder ai-mode "New" sideways
    same "$c" folder sort /
    same "$c" folder sort Salt
    same "$c" note new "From an argument"
    same "$c" --stdin $'From stdin\n\n! Typed action due 2026-12-24\nlast line' note new --folder New -
    same "$c" note new --folder Nope "x"
    same "$c" note new --json
    same "$c" --stdin $'appended line\n! appended action' note append 4 -
    same "$c" note append 4 "another line"
    same "$c" note append 999 "x"
    same "$c" note set 4 $'Replaced title\n\nReplaced body\n! replaced action due 2026-11-11'
    same "$c" note move 4 Salt/Meetings
    same "$c" note move 4 5 /
    same "$c" note move 4 Nope
    same "$c" note tag 4 fresh
    same "$c" note tag 4 fresh
    same "$c" note untag 1 urgent
    same "$c" note untag 1 nosuch
    same "$c" note pin 2 4
    same "$c" note unpin 1
    same "$c" note delete 2
    same "$c" note restore 2 6
    same "$c" note delete --permanent 5
    same "$c" note delete 999
    same "$c" action done 1
    same "$c" action done 1:1
    same "$c" action undone 1:2
    same "$c" action due 1:0 2027-01-15
    same "$c" action due 1:0 13/45/99
    same "$c" action due 1 -
    same "$c" action text 1 "Renamed by uid"
    same "$c" --stdin "Renamed from stdin due 2027-02-02" action text 1:1 -
    same "$c" action text 1 ""
    same "$c" action text 1 $'two\nlines'
    same "$c" action done 999
    same "$c" folder delete New
    same "$c" trash list --json
    same "$c" folder restore 999
    same "$c" folder delete --permanent Deep
    same "$c" tag delete fresh
    same "$c" tag delete nosuch
    same "$c" trash empty
    same "$c" trash empty --yes
    same "$c" backup snapshot.db
    same "$c" note set-modified 1 1790000000
    same "$c" note set-modified 1 notatime
    # The two builds ran these mutations seconds apart, and listings order
    # by modified time: give every note the same, distinct time in both
    # databases so the reads that follow cannot differ by a tick.
    local id
    for id in 1 2 3 4 5 6 7 8 9 10 11 12; do
        fork "$c" note set-modified "$id" $((1790200000 + id * 60)) >/dev/null 2>&1
        gtk  "$c" note set-modified "$id" $((1790200000 + id * 60)) >/dev/null 2>&1
    done
}

# ── Case A: seeded by this build (rich blocks, images) ──────────────────────
fork_case seed
seed fork seed
fork seed note new "Rich note" >/dev/null                           # note 8
env -i PATH=/usr/bin:/bin NOTES_DATA_DIR="$G/fork_seed" \
    "$DEV_SEED" rich 8 "$ROOT/icons/folder.png" >/dev/null \
    || die "dev_seed failed"
fork seed note add-image 1 "$ROOT/icons/file.png" >/dev/null
fork seed note set-modified 8 1790100000 >/dev/null
fork seed note set-modified 1 1790003600 >/dev/null
fork_case a
gtk_case a
cp "$G/fork_seed/notes.db" "$G/fork_a/notes.db"
cp "$G/fork_seed/notes.db" "$G/gtk_a/db/notes.db"
gtk_confirm a
reads a
same a note image 8 2 image8.png;  same_tree a image8.png
same_tree a image1.png
mutations a
MASK_TIMES=1; reads a; MASK_TIMES=0

# ── Case B: seeded by the GTK build ─────────────────────────────────────────
gtk_case seed
gtk_confirm seed
seed gtk seed
fork_case b
gtk_case b
cp "$G/gtk_seed/db/notes.db" "$G/fork_b/notes.db"
cp "$G/gtk_seed/db/notes.db" "$G/gtk_b/db/notes.db"
gtk_confirm b
reads b
mutations b
MASK_TIMES=1; reads b; MASK_TIMES=0

# ── The fallbacks were never taken ──────────────────────────────────────────
if [ -n "$(find "$G/home" -type f 2>/dev/null)" ]; then
    echo "FAIL the GTK reference wrote under its redirected HOME:"
    find "$G/home" -type f
    failed=$((failed + 1))
fi

echo "golden: $passed comparisons identical, $failed different"
[ "$failed" = 0 ]
