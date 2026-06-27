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
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

fail=0
sections=$(grep -oE '^\s*PROF_[A-Z0-9_]+' prof_sections.h | tr -d ' \t')

for s in $sections; do
    case "$s" in
        PROF_SECTION_COUNT) continue ;;          # array size, not a section
        *_LAST) continue ;;                      # range alias
    esac
    if ! grep -rqE "prof_begin\(\s*${s}\s*\)" gl_repl.c src/; then
        echo "ERROR: catalog section ${s} has no prof_begin() site (zombie row" >&2
        echo "       in the Compute Profile panel). Instrument it or remove it" >&2
        echo "       from prof_sections.h + the glr_prof.c tables." >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "prof-sections-instrumented OK ($(echo "$sections" | wc -l | tr -d ' ') catalog rows checked)"
