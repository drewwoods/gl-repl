#!/bin/bash
# Hard guard: src/scene/ must not include from upper layers.
#
# The scene renderer (src/scene/) is layer 3.5. It is allowed to
# include from src/repl/ (small allow-list: command.h, flatten.h, and
# similar program-model types it walks) and src/support/, but must
# stay free of upper-layer headers:
#
#   - src/app/    (controller layer 6)
#   - src/editor/ (text-document model layer 3)
#   - src/ui/     (UI layer 4 — check-layer-coupling enforces UI ↔
#                  scene mutual exclusion; this guard re-asserts it)
#   - src/subsystems/ (feature peers layer 5)
#
# Inverting any of these would mean the scene renderer reaches into
# the controller / editor / UI / feature subsystems — a layering
# inversion that breaks the standalone scene_demo target's reason
# for existing (it links src/scene/ without the rest of the tree).
#
# Comment-only references are filtered out — they describe history
# but do not link.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

forbidden='#include[[:space:]]+"(app|editor|ui|subsystems)/'

files=()
while IFS= read -r f; do
  files+=( "$f" )
done < <(find src/scene -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
  echo "ERROR: src/scene/ contains no .c / .h files — guard cannot run." >&2
  exit 1
fi

hits="$( { grep -nE "$forbidden" "${files[@]}" 2>/dev/null || true; } \
        | grep -vE ':[[:space:]]*\*|:[[:space:]]*//|:[[:space:]]*/\*' \
        || true )"

if [ -n "$hits" ]; then
  echo "ERROR: src/scene/ contains forbidden upper-layer #includes:" >&2
  echo "$hits" >&2
  echo >&2
  echo "       Scene code must not depend on app/, editor/, ui/, or" >&2
  echo "       subsystems/. Move the symbol into src/repl/ or" >&2
  echo "       src/support/ if it is genuinely scene-relevant, or" >&2
  echo "       hoist the call site into src/app/glr_ctrl.c." >&2
  exit 1
fi

echo "scene-no-upper-layers OK (no upper-layer includes)"
