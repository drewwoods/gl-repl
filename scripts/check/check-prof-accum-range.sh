#!/bin/bash
# Hard guard: sections inside the render3d accumulation index range must
# publish with prof_accum_end(), never prof_end().
#
# glr_ctrl_display_frame() sweeps PROF_RENDER3D_SETUP .. PROF_RENDER3D_LAST
# with prof_accum_reset() before the pass and prof_accum_commit() after it,
# so that a scene rendered as several accumulation samples reports the SUM of
# its passes rather than whichever one happened to finish last.
#
# A section in that range instrumented with prof_end() is silently broken,
# and in a direction no reviewer notices: prof_end() publishes a real number
# immediately, then the frame's prof_accum_commit() overwrites it with the
# accumulator - which prof_accum_end() was never called to fill. The row
# reads "--" forever. It looks exactly like a section whose body did not run,
# which is a legitimate state for half the rows in this range (the backdrop
# is off, the fade ring is empty), so nothing about the panel gives it away.
#
# The range membership is what makes it a trap: a section can be instrumented
# anywhere in the tree, far from the render3d files whose neighbours would
# have shown the right idiom.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

catalog=prof_sections.h
if [ ! -f "$catalog" ]; then
  echo "ERROR: $catalog not found — guard cannot run." >&2
  exit 1
fi

# The accumulated range: enum constants from PROF_RENDER3D_SETUP up to the
# row PROF_RENDER3D_LAST aliases. Read from the catalog so the list cannot
# drift from the sweep in glr_ctrl.c.
names="$(awk '
  /PROF_RENDER3D_SETUP,/           { collecting = 1 }
  collecting && match($0, /PROF_[A-Z0-9_]+,/) {
      print substr($0, RSTART, RLENGTH - 1)
  }
  /PROF_RENDER3D_LAST[[:space:]]*=/ { exit }
' "$catalog")"

count="$(printf '%s\n' "$names" | grep -c . || true)"
if [ "$count" -lt 5 ]; then
  echo "ERROR: parsed only $count sections from $catalog — guard cannot run." >&2
  exit 1
fi

hits=""
while IFS= read -r name; do
  [ -n "$name" ] || continue
  found="$( { grep -rnE "prof_end[[:space:]]*\([[:space:]]*$name[[:space:]]*\)" \
                src gl_repl.c tools 2>/dev/null || true; } \
            | grep -vE 'prof_accum_end' || true )"
  if [ -n "$found" ]; then
    hits="$hits$found"$'\n'
  fi
done <<< "$names"

if [ -n "$hits" ]; then
  echo "ERROR: accumulated render3d sections closed with prof_end():" >&2
  printf '%s' "$hits" >&2
  echo >&2
  echo "       Sections between PROF_RENDER3D_SETUP and PROF_RENDER3D_LAST" >&2
  echo "       are swept by prof_accum_reset/_commit in the frame, so they" >&2
  echo "       must close with prof_accum_end(). prof_end() publishes a" >&2
  echo "       value the frame's commit then overwrites with an empty" >&2
  echo "       accumulator, and the row reads \"--\" forever." >&2
  exit 1
fi

echo "prof-accum-range OK ($count accumulated sections, all closed with prof_accum_end)"
