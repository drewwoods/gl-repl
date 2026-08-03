#!/bin/bash
# Hard guard: src/ui/core/ must not include from upper layers.
#
# src/ui/core/ holds the generic UI primitives (text layout, hit
# testing, theme, tabbed-overlay shell) shared across the app-coupled
# UI shells in src/ui/app/ and tests. To keep these primitives
# substitutable, ui/core must not include from:
#
#   - src/app/        (controller layer 6)
#   - src/editor/     (text-document model layer 3)
#   - src/repl/       (program model layer 1)
#   - src/render3d/      (render3d renderer layer 3.5 -
#                      check-layer-coupling enforces UI <--> render3d
#                      mutual exclusion overall; this re-asserts it
#                      for the core subset)
#   - src/subsystems/ (feature peers layer 5)
#   - src/ui/app/     (app-coupled UI shells layer 4.5 —
#                      core must not depend on app)
#
# ui/core's allowed dependencies are src/support/, src/include/, and
# its own internal headers.
#
# Comment-only references are filtered out — they describe history
# but do not link.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

forbidden='#include[[:space:]]+"(app|editor|repl|render3d|subsystems|ui/app)/'

files=()
while IFS= read -r f; do
  files+=( "$f" )
done < <(find src/ui/core -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "ERROR: src/ui/core/ contains no .c / .h files — guard cannot run." >&2
  exit 1
fi

hits="$( { grep -nE "$forbidden" "${files[@]}" 2>/dev/null || true; } \
        | grep -vE ':[[:space:]]*\*|:[[:space:]]*//|:[[:space:]]*/\*' \
        || true )"

if [ -n "$hits" ]; then
  echo "ERROR: src/ui/core/ contains forbidden upper-layer #includes:" >&2
  echo "$hits" >&2
  echo >&2
  echo "       ui/core must stay independent of editor / repl / render3d /" >&2
  echo "       subsystems / ui/app / app. Move the dependency into" >&2
  echo "       src/ui/app/ or expose the data via a parameter." >&2
  exit 1
fi

echo "ui-core-no-upper-layers OK (no upper-layer includes)"
