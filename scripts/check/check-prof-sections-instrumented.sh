#!/bin/bash
# check-prof-sections-instrumented.sh - no zombie profiling sections.
#
# Every section in the prof_sections.h catalog must have at least one
# prof_begin(SECTION) instrumentation site in the shipped sources (gl_repl.c
# + src/). A catalog row with no probe renders as a permanently-empty "--"
# line in the Compute Profile panel — exactly what happened when the text
# panel refactor deleted the old code-panel renderer together with its
# prof_begin/prof_end calls while the catalog kept the rows (zombies from
# May 2026 until the audit that added this guard).
#
# prof_accum_end/_commit/_reset don't count as the entry point — the accum
# pattern still opens every span with prof_begin, so matching prof_begin
# alone covers both plain and accumulated sections.
#
# prof_section_record_us() does count: a derived row (PROF_PRESENT, which is
# the frame total minus the frame's work) has no span to bracket, but it is
# instrumented — it lands a sample every frame, which is all this guard is
# asking about.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
. scripts/lib/rg-fallback.sh

fail=0
sections=$(grep -oE '^\s*PROF_[A-Z0-9_]+' prof_sections.h | tr -d ' \t')

# One recursive scan collects every instrumented section name, instead of a
# fresh tree-wide search per catalog row - the previous O(sections * tree)
# shape made this the slowest state-ownership guard (~4.7s standalone).
# `rg`/`ag` (scripts/lib/rg-fallback.sh, preferred over plain grep) cut the
# cost of the single scan itself too.
raw="$(rg -o 'prof_(begin|section_record_us)\(\s*PROF_[A-Z0-9_]+' gl_repl.c src/ 2>/dev/null || true)"
instrumented="$(printf '%s\n' "$raw" | grep -oE 'PROF_[A-Z0-9_]+' | sort -u)"

for s in $sections; do
    case "$s" in
        PROF_SECTION_COUNT) continue ;;          # array size, not a section
        *_LAST) continue ;;                      # range alias
    esac
    if ! printf '%s\n' "$instrumented" | grep -qxF "$s"; then
        echo "ERROR: catalog section ${s} has no prof_begin() or" >&2
        echo "       prof_section_record_us() site (zombie row in the Compute" >&2
        echo "       Profile panel). Instrument it or remove it from" >&2
        echo "       prof_sections.h + the glr_prof.c tables." >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "prof-sections-instrumented OK ($(echo "$sections" | wc -l | tr -d ' ') catalog rows checked)"
